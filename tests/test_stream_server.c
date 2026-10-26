// The preview stream runs on its own thread so that a viewer on a bad
// tether can only stall the preview, never consumer_loop and the virtual
// camera it feeds. What has to hold: offering a frame never waits on the
// network, a viewer gets complete length-prefixed JPEGs, and a viewer that
// stops reading is dropped within roughly send_timeout_ms per frame.
#define _POSIX_C_SOURCE 200809L
#include "stream_server.c"
#include "test_harness.h"
#include <netdb.h>
#include <sys/time.h>

static uint16_t port;

// A port nothing is listening on, from the kernel's ephemeral range. Picked
// per test: the suite may run alongside other copies of itself (repeated
// runs in parallel), so a fixed or pid-derived port can collide.
static uint16_t pick_port(void) {
  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  struct sockaddr_in6 addr = {.sin6_family = AF_INET6,
                              .sin6_addr = in6addr_any};
  socklen_t len = sizeof addr;
  uint16_t p = 0;
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0 &&
      getsockname(fd, (struct sockaddr *)&addr, &len) == 0)
    p = ntohs(addr.sin6_port);
  close(fd);
  return p;
}

// Opens a server on a fresh port, or fails the test cleanly.
static StreamServer *open_server(void) {
  port = pick_port();
  StreamServer *s = port ? stream_server_open(port) : NULL;
  CHECK(s != NULL);
  return s;
}

static double now_ms_d(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

// IPv4 loopback: the server listens dual-stack, and not every test host
// has ::1 configured.
static int connect_viewer(int rcvbuf) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (rcvbuf > 0)
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                             .sin_port = htons(port)};
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static bool wait_viewer(StreamServer *s, bool want) {
  for (int i = 0; i < 300; i++) {
    if (__atomic_load_n(&s->viewer, __ATOMIC_SEQ_CST) == want)
      return true;
    sleep_ms(10);
  }
  return false;
}

static bool read_exact(int fd, uint8_t *buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = recv(fd, buf + got, n - got, 0);
    if (r <= 0)
      return false;
    got += (size_t)r;
  }
  return true;
}

// Noise compresses badly, which is what makes a frame big enough to fill
// a small socket buffer in the slow-viewer test.
static VideoFrame *noise_frame(uint32_t w, uint32_t h) {
  VideoFrame *f = vf_create(w, h, 0);
  unsigned s = 7;
  size_t total = f->plane_sizes[0] + f->plane_sizes[1] + f->plane_sizes[2];
  for (size_t i = 0; i < total; i++) {
    s = s * 1103515245u + 12345u;
    f->pixel_data[i] = (uint8_t)(s >> 16);
  }
  return f;
}

static void test_no_viewer_means_no_work(void) {
  StreamServer *s = open_server();
  if (!s)
    return;
  VideoFrame *f = noise_frame(64, 32);
  CHECK(!stream_server_offer_frame(s, f, 60));
  CHECK_EQ_INT(s->buf_cap, 0); // nothing even allocated
  vf_free(f);
  stream_server_close(s);
}

static void test_viewer_receives_complete_jpegs(void) {
  StreamServer *s = open_server();
  if (!s)
    return;
  int fd = connect_viewer(0);
  CHECK(fd >= 0);
  CHECK(wait_viewer(s, true));

  VideoFrame *f = noise_frame(64, 32);
  for (int i = 0; i < 3; i++) {
    CHECK(stream_server_offer_frame(s, f, 80));
    uint8_t len_be[4];
    CHECK(read_exact(fd, len_be, 4));
    uint32_t len = ((uint32_t)len_be[0] << 24) | ((uint32_t)len_be[1] << 16) |
                   ((uint32_t)len_be[2] << 8) | len_be[3];
    CHECK(len > 4 && len < 1u << 20);
    if (len <= 4 || len >= 1u << 20) {
      printf("    got length prefix %02x %02x %02x %02x\n", len_be[0],
             len_be[1], len_be[2], len_be[3]);
      break;
    }
    uint8_t *jpeg = malloc(len);
    CHECK(read_exact(fd, jpeg, len));
    CHECK(jpeg[0] == 0xFF && jpeg[1] == 0xD8);
    CHECK(jpeg[len - 2] == 0xFF && jpeg[len - 1] == 0xD9);
    tjhandle d = tjInitDecompress();
    int w = 0, h = 0, sub = 0, cs = 0;
    CHECK(tjDecompressHeader3(d, jpeg, len, &w, &h, &sub, &cs) == 0);
    CHECK_EQ_INT(w, 64);
    CHECK_EQ_INT(h, 32);
    tjDestroy(d);
    free(jpeg);
  }
  vf_free(f);
  close(fd);
  stream_server_close(s);
}

// A viewer that connects and never reads. Every offer must still return
// at once, and the server must give up on the viewer rather than keep
// trying forever.
static void test_stalled_viewer_never_blocks_offer(void) {
  StreamServer *s = open_server();
  if (!s)
    return;
  int fd = connect_viewer(4096);
  CHECK(fd >= 0);
  CHECK(wait_viewer(s, true));

  VideoFrame *f = noise_frame(640, 480); // ~1MB per JPEG at quality 100
  double worst = 0;
  double start = now_ms_d();
  bool dropped = false;
  while (now_ms_d() - start < 3000) {
    double t0 = now_ms_d();
    stream_server_offer_frame(s, f, 100);
    double dt = now_ms_d() - t0;
    if (dt > worst)
      worst = dt;
    if (!__atomic_load_n(&s->viewer, __ATOMIC_SEQ_CST)) {
      dropped = true;
      break;
    }
    sleep_ms(33);
  }
  CHECK(dropped);
  // The offer is a memcpy of a 600KB frame. Waiting on the socket would
  // mean waiting out the 200ms send deadline; the bound below that only
  // leaves room for scheduling delays on a loaded machine.
  CHECK(worst < 150.0);
  printf("    (worst offer %.2f ms, viewer dropped after %.0f ms)\n", worst,
         now_ms_d() - start);
  vf_free(f);
  close(fd);
  stream_server_close(s);
}

// Last connection wins -- the replaced viewer sees its connection closed
// -- and close() returns promptly with a viewer still attached.
static void test_new_viewer_replaces_old_and_close_is_prompt(void) {
  StreamServer *s = open_server();
  if (!s)
    return;
  int a = connect_viewer(0);
  CHECK(wait_viewer(s, true));
  int b = connect_viewer(0);
  struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
  setsockopt(a, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  char byte;
  CHECK(recv(a, &byte, 1, 0) == 0); // EOF: the server dropped viewer a
  // ...and b is the viewer, with no gap.
  CHECK(__atomic_load_n(&s->viewer, __ATOMIC_SEQ_CST));
  double t0 = now_ms_d();
  stream_server_close(s);
  CHECK(now_ms_d() - t0 < 1000.0);
  close(a);
  close(b);
}

int main(void) {
  printf("test_stream_server:\n");
  RUN_TEST(test_no_viewer_means_no_work);
  RUN_TEST(test_viewer_receives_complete_jpegs);
  RUN_TEST(test_stalled_viewer_never_blocks_offer);
  RUN_TEST(test_new_viewer_replaces_old_and_close_is_prompt);
  TEST_MAIN_END();
}
