// _POSIX_C_SOURCE before any header include: -std=c23 puts glibc in strict
// ISO mode, which hides several socket-adjacent declarations unless a
// feature-test macro asks for them explicitly. Same guard as
// config.c/eeye.c/v4l2_in.c.
#define _POSIX_C_SOURCE 200809L

#include "stream_server.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <turbojpeg.h>
#include <unistd.h>

// How long the stream thread gives one frame to reach the viewer, in
// total, before dropping that viewer as too slow. A per-frame deadline
// rather than a per-send() timeout: SO_SNDTIMEO restarts on every partial
// send, so a viewer draining slowly could hold a frame far past it. This no
// longer bounds the virtual camera (sending is off consumer_loop's thread
// now), only how stale the preview is allowed to get.
static constexpr int send_timeout_ms = 200;

// How often the stream thread wakes with nothing to do, to notice a new
// viewer or shutdown.
static constexpr int idle_poll_ms = 200;

// One frame's planes, copied out of the pipeline so the frame itself can go
// straight back to the pool.
typedef struct {
  uint8_t *data;
  uint32_t width;
  uint32_t height;
  int strides[3];
  size_t plane_sizes[3];
  int quality;
} PreviewFrame;

struct StreamServer {
  int listen_fd;
  int client_fd; // -1 when no client is connected; stream thread only
  // Mirrors client_fd >= 0 for the offering thread, so an unwatched stream
  // costs it one atomic load per frame and no copy.
  atomic_bool viewer;
  tjhandle encoder;
  unsigned char *jpeg_buf; // Reused across frames -- see encode_and_send.
  unsigned long jpeg_buf_cap;

  pthread_t thread;
  atomic_bool quit;

  // The mailbox. `pending` is written by stream_server_offer_frame and
  // swapped with `working` by the stream thread, both under `lock`; the
  // stream thread encodes `working` without it.
  pthread_mutex_t lock;
  pthread_cond_t ready;
  PreviewFrame pending;
  PreviewFrame working;
  size_t buf_cap; // size of both data buffers; fixed after the first frame
  bool has_pending;
};

static void *stream_loop(void *arg);

StreamServer *stream_server_open(uint16_t port) {
  // AF_INET6 rather than AF_INET, with IPV6_V6ONLY off below: one socket
  // then accepts both IPv6 and IPv4 clients (the latter arriving as
  // v4-mapped addresses), so this listens on everything with a single fd.
  //
  // This matters for the deployment this project actually targets. On a
  // direct Ethernet tether there is no DHCP server and no DNS, so the only
  // addressing that works with zero configuration is IPv6 link-local
  // (fe80::/64), which every host self-assigns on every live link. An
  // IPv4-only listener is simply unreachable that way -- which is exactly
  // why the tether used to need manual address setup on both ends.
  int fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    printf("stream_server: socket() failed: %s\n", strerror(errno));
    return NULL;
  }

  // So a restart doesn't fail to rebind while the old socket lingers in
  // TIME_WAIT -- purely a convenience for restarting during development,
  // not a correctness requirement.
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  // Dual-stack. Linux defaults this off (net.ipv6.bindv6only=0) but that is
  // a sysctl an operator can flip, so set it explicitly rather than inherit
  // whatever the host happens to be configured for. Not fatal if it fails:
  // the socket still serves IPv6, which is the case that needs it most.
  int v6only = 0;
  if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof v6only) < 0) {
    printf("stream_server: could not enable dual-stack on port %u (%s); "
           "IPv4 clients may not be able to connect\n",
           port, strerror(errno));
  }

  struct sockaddr_in6 addr = {0};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  addr.sin6_port = htons(port);

  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
    printf("stream_server: bind() on port %u failed: %s\n", port,
           strerror(errno));
    close(fd);
    return NULL;
  }

  // backlog of 1: this is a single-viewer server (see accept_pending()) --
  // a second simultaneous connection attempt can simply wait or be refused
  // by the kernel rather than needing space queued up for it here.
  if (listen(fd, 1) < 0) {
    printf("stream_server: listen() failed: %s\n", strerror(errno));
    close(fd);
    return NULL;
  }

  tjhandle encoder = tjInitCompress();
  if (!encoder) {
    printf("stream_server: tjInitCompress failed: %s\n", tjGetErrorStr());
    close(fd);
    return NULL;
  }

  StreamServer *s = (StreamServer *)calloc(1, sizeof(StreamServer));
  if (!s) {
    tjDestroy(encoder);
    close(fd);
    return NULL;
  }
  s->listen_fd = fd;
  s->client_fd = -1;
  s->encoder = encoder;
  atomic_init(&s->viewer, false);
  atomic_init(&s->quit, false);
  pthread_mutex_init(&s->lock, NULL);
  // Monotonic, so the idle wake-up below isn't stretched by a wall-clock
  // step (a Pi has no RTC).
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&s->ready, &attr);
  pthread_condattr_destroy(&attr);

  if (pthread_create(&s->thread, NULL, stream_loop, s) != 0) {
    printf("stream_server: could not start the stream thread\n");
    pthread_cond_destroy(&s->ready);
    pthread_mutex_destroy(&s->lock);
    tjDestroy(encoder);
    close(fd);
    free(s);
    return NULL;
  }

  printf("stream_server: listening on port %u\n", port);
  return s;
}

static void drop_client(StreamServer *s) {
  if (s->client_fd >= 0) {
    close(s->client_fd);
    s->client_fd = -1;
  }
  atomic_store(&s->viewer, false);
}

// Accepts at most one pending connection per call. Called on every pass
// of the stream loop -- at least every idle_poll_ms, and once per frame
// while frames flow -- so a freshly-connected viewer is picked up within a
// frame or two.
static void accept_pending(StreamServer *s) {
  int fd = accept(s->listen_fd, NULL, NULL);
  if (fd < 0)
    return; // EAGAIN/EWOULDBLOCK: nothing pending, the common case

  drop_client(s); // single-viewer server: last connection wins

  // Non-blocking, so send_frame below can hold every send to one deadline
  // per frame.
  int flags = fcntl(fd, F_GETFL);
  if (flags >= 0)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  // Nagle could hold back the tail of a frame waiting for an ACK, adding
  // latency to a stream that is otherwise sent as soon as each frame is
  // ready. There's no coalescing upside to lose here -- each frame already
  // goes out as one sendmsg() of the length prefix plus the JPEG.
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  s->client_fd = fd;
  atomic_store(&s->viewer, true);
  printf("stream_server: viewer connected\n");
}

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Sends the length prefix and the JPEG as one sendmsg() stream, giving the
// whole frame send_timeout_ms. Returns false if the viewer failed or ran
// out of time.
static bool send_frame(int fd, const uint8_t *hdr, size_t hdr_len,
                       const uint8_t *body, size_t body_len) {
  const int64_t deadline = now_ms() + send_timeout_ms;
  size_t sent = 0;
  const size_t total = hdr_len + body_len;
  while (sent < total) {
    struct iovec iov[2];
    int n_iov = 0;
    if (sent < hdr_len) {
      iov[n_iov++] = (struct iovec){(void *)(hdr + sent), hdr_len - sent};
      iov[n_iov++] = (struct iovec){(void *)body, body_len};
    } else {
      size_t off = sent - hdr_len;
      iov[n_iov++] = (struct iovec){(void *)(body + off), body_len - off};
    }
    struct msghdr msg = {.msg_iov = iov, .msg_iovlen = (size_t)n_iov};
    // MSG_NOSIGNAL: a viewer that vanishes mid-write would otherwise raise
    // SIGPIPE -- eeye.c already ignores that process-wide for the same
    // reason, but making the socket call itself not raise it is one fewer
    // thing to reason about here.
    ssize_t n = sendmsg(fd, &msg, MSG_NOSIGNAL);
    if (n > 0) {
      sent += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      int64_t left = deadline - now_ms();
      if (left <= 0)
        return false;
      struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
      poll(&pfd, 1, (int)left);
      continue;
    }
    return false;
  }
  return true;
}

static void encode_and_send(StreamServer *s, const PreviewFrame *f) {
  const unsigned char *planes[3] = {
      f->data, f->data + f->plane_sizes[0],
      f->data + f->plane_sizes[0] + f->plane_sizes[1]};

  // Reused across every streamed frame instead of letting
  // tjCompressFromYUVPlanes malloc/free a fresh buffer each call -- frame
  // dimensions never change mid-run in this pipeline, so after the first
  // call this is just a bounds check, not a real allocation.
  unsigned long needed = tjBufSize((int)f->width, (int)f->height, TJSAMP_422);
  if (s->jpeg_buf_cap < needed) {
    if (s->jpeg_buf)
      tjFree(s->jpeg_buf);
    s->jpeg_buf = tjAlloc((int)needed);
    s->jpeg_buf_cap = s->jpeg_buf ? needed : 0;
  }
  if (!s->jpeg_buf)
    return; // Allocation failed; nothing to send this frame.

  unsigned long jpeg_size = 0;
  if (tjCompressFromYUVPlanes(s->encoder, planes, (int)f->width, f->strides,
                              (int)f->height, TJSAMP_422, &s->jpeg_buf,
                              &jpeg_size, f->quality,
                              TJFLAG_FASTDCT | TJFLAG_NOREALLOC) < 0) {
    printf("stream_server: tjCompressFromYUVPlanes failed: %s\n",
           tjGetErrorStr2(s->encoder));
    return;
  }

  uint32_t len_be = htonl((uint32_t)jpeg_size);
  if (!send_frame(s->client_fd, (const uint8_t *)&len_be, sizeof len_be,
                  s->jpeg_buf, jpeg_size)) {
    printf("stream_server: viewer disconnected (or too slow); dropping\n");
    drop_client(s);
  }
}

static void *stream_loop(void *arg) {
  StreamServer *s = arg;
  while (!atomic_load(&s->quit)) {
    accept_pending(s);
    if (s->client_fd < 0) {
      // Nobody watching: sleep on the listening socket itself.
      struct pollfd pfd = {.fd = s->listen_fd, .events = POLLIN, .revents = 0};
      poll(&pfd, 1, idle_poll_ms);
      continue;
    }

    bool got = false;
    pthread_mutex_lock(&s->lock);
    if (!s->has_pending) {
      struct timespec until;
      clock_gettime(CLOCK_MONOTONIC, &until);
      until.tv_nsec += (long)idle_poll_ms * 1000000L;
      if (until.tv_nsec >= 1000000000L) {
        until.tv_sec += 1;
        until.tv_nsec -= 1000000000L;
      }
      pthread_cond_timedwait(&s->ready, &s->lock, &until);
    }
    if (s->has_pending) {
      PreviewFrame t = s->working;
      s->working = s->pending;
      s->pending = t;
      s->has_pending = false;
      got = true;
    }
    pthread_mutex_unlock(&s->lock);

    if (got && s->client_fd >= 0)
      encode_and_send(s, &s->working);
  }
  drop_client(s);
  return NULL;
}

bool stream_server_offer_frame(StreamServer *s, const VideoFrame *frame,
                               int quality) {
  if (!s || !atomic_load(&s->viewer))
    return false; // no one watching -- skip the copy entirely

  const size_t total =
      frame->plane_sizes[0] + frame->plane_sizes[1] + frame->plane_sizes[2];

  pthread_mutex_lock(&s->lock);
  if (s->buf_cap == 0) {
    // First frame: size both buffers. Geometry is fixed for the run, and
    // the stream thread cannot be holding `working` before any frame has
    // been offered, so this is the one safe moment to allocate it.
    s->pending.data = malloc(total);
    s->working.data = malloc(total);
    if (!s->pending.data || !s->working.data) {
      free(s->pending.data);
      free(s->working.data);
      s->pending.data = s->working.data = NULL;
      pthread_mutex_unlock(&s->lock);
      return false;
    }
    s->buf_cap = total;
  }
  if (total > s->buf_cap) {
    pthread_mutex_unlock(&s->lock);
    return false; // can't happen: frame geometry never changes mid-run
  }

  // frame->planes: the post-effects buffer, i.e. exactly what the virtual
  // camera is also showing -- unless the caller passed a raw view (see
  // stream_raw in config.h).
  PreviewFrame *p = &s->pending;
  size_t off = 0;
  for (int i = 0; i < 3; ++i) {
    memcpy(p->data + off, frame->planes[i], frame->plane_sizes[i]);
    p->plane_sizes[i] = frame->plane_sizes[i];
    p->strides[i] = (int)frame->stride[i];
    off += frame->plane_sizes[i];
  }
  p->width = frame->width;
  p->height = frame->height;
  p->quality = quality;
  s->has_pending = true;
  pthread_cond_signal(&s->ready);
  pthread_mutex_unlock(&s->lock);
  return true;
}

void stream_server_close(StreamServer *s) {
  if (!s)
    return;

  atomic_store(&s->quit, true);
  pthread_mutex_lock(&s->lock);
  pthread_cond_signal(&s->ready);
  pthread_mutex_unlock(&s->lock);
  pthread_join(s->thread, NULL);

  if (s->listen_fd >= 0)
    close(s->listen_fd);
  if (s->encoder)
    tjDestroy(s->encoder);
  if (s->jpeg_buf)
    tjFree(s->jpeg_buf);
  free(s->pending.data);
  free(s->working.data);
  pthread_cond_destroy(&s->ready);
  pthread_mutex_destroy(&s->lock);
  free(s);
}
