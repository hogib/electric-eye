// _POSIX_C_SOURCE before any header include: -std=c23 puts glibc in strict
// ISO mode, which hides several socket-adjacent declarations unless a
// feature-test macro asks for them explicitly. Same guard as
// config.c/eeye.c/v4l2_in.c.
#define _POSIX_C_SOURCE 200809L

#include "stream_server.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <turbojpeg.h>
#include <unistd.h>

// Bounds how long one stream_server_send_frame() call can block on a
// stalled network peer before giving up on that connection. This runs on
// consumer_loop's thread, right alongside the locally-critical virtual-cam
// write -- a wedged topside client must never be able to stall the real
// pipeline for longer than this.
static constexpr int send_timeout_ms = 200;

struct StreamServer {
  int listen_fd;
  int client_fd; // -1 when no client is connected
  tjhandle encoder;
  unsigned char *jpeg_buf; // Reused across frames -- see stream_server_send_frame.
  unsigned long jpeg_buf_cap;
};

StreamServer *stream_server_open(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    printf("stream_server: socket() failed: %s\n", strerror(errno));
    return NULL;
  }

  // So a restart doesn't fail to rebind while the old socket lingers in
  // TIME_WAIT -- purely a convenience for restarting during development,
  // not a correctness requirement.
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

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

  printf("stream_server: listening on port %u\n", port);
  return s;
}

static void drop_client(StreamServer *s) {
  if (s->client_fd >= 0) {
    close(s->client_fd);
    s->client_fd = -1;
  }
}

// Accepts at most one pending connection per call -- called once per frame
// that reaches stream_server_send_frame(), which is plenty often for a
// freshly-connected client to be picked up within a frame or two.
static void accept_pending(StreamServer *s) {
  int fd = accept(s->listen_fd, NULL, NULL);
  if (fd < 0)
    return; // EAGAIN/EWOULDBLOCK: nothing pending, the common case

  drop_client(s); // single-viewer server: last connection wins

  struct timeval tv = {.tv_sec = send_timeout_ms / 1000,
                       .tv_usec = (send_timeout_ms % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

  // Every frame goes out as two send() calls (a 4-byte length prefix, then
  // the JPEG body -- see send_all's two calls below); without this, Nagle's
  // algorithm can hold that tiny first packet back waiting to coalesce with
  // more data or an ACK, adding real per-frame latency to a stream that's
  // otherwise sent as soon as each frame is ready. There's no coalescing
  // upside to lose here -- these are already the largest writes this socket
  // will ever make, not a stream of tiny writes Nagle is meant to batch.
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  s->client_fd = fd;
  printf("stream_server: viewer connected\n");
}

static bool send_all(int fd, const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    // MSG_NOSIGNAL: a viewer that vanishes mid-write would otherwise raise
    // SIGPIPE -- eeye.c already ignores that process-wide for the same
    // reason, but making the socket call itself not raise it is one fewer
    // thing to reason about here.
    ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false; // includes EAGAIN/EWOULDBLOCK once SO_SNDTIMEO expires
    }
    sent += (size_t)n;
  }
  return true;
}

bool stream_server_send_frame(StreamServer *s, const VideoFrame *frame,
                              int quality) {
  if (!s)
    return false;

  accept_pending(s);
  if (s->client_fd < 0)
    return false; // no one watching -- skip the JPEG work entirely

  // frame->planes (not raw_planes): the post-effects buffer, i.e. exactly
  // what the virtual camera is also showing -- what you'd see locally is
  // what streams.
  const unsigned char *planes[3] = {frame->planes[0], frame->planes[1],
                                    frame->planes[2]};
  int strides[3] = {(int)frame->stride[0], (int)frame->stride[1],
                    (int)frame->stride[2]};

  // Reused across every streamed frame instead of letting
  // tjCompressFromYUVPlanes malloc/free a fresh buffer each call -- frame
  // dimensions never change mid-run in this pipeline, so after the first
  // call this is just a bounds check, not a real allocation.
  unsigned long needed =
      tjBufSize((int)frame->width, (int)frame->height, TJSAMP_422);
  if (s->jpeg_buf_cap < needed) {
    if (s->jpeg_buf)
      tjFree(s->jpeg_buf);
    s->jpeg_buf = tjAlloc((int)needed);
    s->jpeg_buf_cap = s->jpeg_buf ? needed : 0;
  }
  if (!s->jpeg_buf)
    return false; // Allocation failed; nothing to send this frame.

  unsigned long jpeg_size = 0;
  if (tjCompressFromYUVPlanes(s->encoder, planes, (int)frame->width, strides,
                              (int)frame->height, TJSAMP_422, &s->jpeg_buf,
                              &jpeg_size, quality,
                              TJFLAG_FASTDCT | TJFLAG_NOREALLOC) < 0) {
    printf("stream_server: tjCompressFromYUVPlanes failed: %s\n",
           tjGetErrorStr2(s->encoder));
    return false;
  }

  uint32_t len_be = htonl((uint32_t)jpeg_size);
  bool ok = send_all(s->client_fd, (const uint8_t *)&len_be, sizeof len_be) &&
           send_all(s->client_fd, s->jpeg_buf, jpeg_size);

  if (!ok) {
    printf("stream_server: viewer disconnected (or too slow); dropping\n");
    drop_client(s);
    return false;
  }

  return true;
}

void stream_server_close(StreamServer *s) {
  if (!s)
    return;

  drop_client(s);
  if (s->listen_fd >= 0)
    close(s->listen_fd);
  if (s->encoder)
    tjDestroy(s->encoder);
  if (s->jpeg_buf)
    tjFree(s->jpeg_buf);
  free(s);
}
