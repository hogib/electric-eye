// hellion's building blocks: its config, pipelines (driven end to end over
// the frame bus, so no camera is needed), the status file top reads, and
// the log capture. Streams live in a private EEYE_BUS_DIR.
#define _POSIX_C_SOURCE 200809L
#include "child_proc.h"
#include "discovery.h"
#include "ffmpeg.h"
#include "fbus.h"
#include "hconfig.h"
#include "hlog.h"
#include "hstatus.h"
#include "pipeline.h"
#include "hrecord.h"
#include "udp_relay.h"
#include "test_harness.h"
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char bus_dir[64];

static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

static bool parse(const char *json, HellionConfig *out) {
  return hconfig_parse(json, strlen(json), out);
}

// ---- Config ---------------------------------------------------------------

static void test_config_full_example(void) {
  HellionConfig c;
  CHECK(parse("{\"name\":\"rov\",\"role\":\"topside\","
              "\"eeye_config\":\"/var/lib/eeye/effects.json\","
              "\"pipelines\":[{\"name\":\"uplink\","
              "\"source\":\"v4l2:/dev/video0\",\"width\":640,\"height\":480,"
              "\"format\":\"yuyv\",\"sinks\":[\"bus:camera\","
              "\"v4l2:/dev/video11\"]},"
              "{\"name\":\"relay\",\"source\":\"bus:pilot\","
              "\"sinks\":[\"bus:pilot2\"]}]}",
              &c));
  CHECK(strcmp(c.name, "rov") == 0);
  CHECK(strcmp(c.role, "topside") == 0);
  CHECK(strcmp(c.eeye_config, "/var/lib/eeye/effects.json") == 0);
  CHECK_EQ_INT(c.pipeline_count, 2);

  const PipelineConfig *p = &c.pipelines[0];
  CHECK(strcmp(p->name, "uplink") == 0);
  CHECK_EQ_INT(p->source, SRC_V4L2);
  CHECK(strcmp(p->source_arg, "/dev/video0") == 0);
  CHECK_EQ_INT(p->width, 640);
  CHECK_EQ_INT(p->format, FORMAT_YUYV);
  CHECK_EQ_INT(p->sink_count, 2);
  CHECK_EQ_INT(p->sinks[0].kind, SINK_BUS);
  CHECK(strcmp(p->sinks[0].arg, "camera") == 0);
  CHECK_EQ_INT(p->sinks[1].kind, SINK_V4L2);
  CHECK(strcmp(p->sinks[1].arg, "/dev/video11") == 0);
  CHECK(strcmp(p->sinks[1].spec, "v4l2:/dev/video11") == 0);

  CHECK_EQ_INT(c.pipelines[1].source, SRC_BUS);
  CHECK(strcmp(c.pipelines[1].source_arg, "pilot") == 0);
}

static void test_config_defaults(void) {
  HellionConfig c;
  CHECK(parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"rpicam\","
              "\"sinks\":[\"bus:x\"]}]}",
              &c));
  CHECK(strcmp(c.name, "hellion") == 0);
  CHECK(strcmp(c.role, "rov") == 0);
  CHECK_EQ_INT(c.eeye_config[0], '\0');
  CHECK_EQ_INT(c.pipelines[0].source, SRC_RPICAM);
  CHECK_EQ_INT(c.pipelines[0].width, 1280);
  CHECK_EQ_INT(c.pipelines[0].height, 720);
  CHECK_EQ_INT(c.pipelines[0].format, FORMAT_AUTO);
}

static void test_config_errors(void) {
  HellionConfig c;
  const char *one = "{\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\","
                    "\"sinks\":[\"bus:b\"]}]}";
  CHECK(parse(one, &c)); // the baseline the cases below break

  CHECK(!parse("{}", &c)); // no pipelines: nothing to do
  CHECK(!parse("{\"pipelines\":[]}", &c));
  CHECK(!parse("{\"bogus\":1,\"pipelines\":[]}", &c));
  CHECK(!parse("{\"role\":\"drone\",\"pipelines\":[]}", &c));
  CHECK(!parse("{\"name\":\"../x\",\"pipelines\":[]}", &c));
  // Pipeline-level problems.
  CHECK(!parse("{\"pipelines\":[{\"source\":\"bus:a\",\"sinks\":[\"bus:b\"]}]}",
               &c)); // no name
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"sinks\":[\"bus:b\"]}]}",
               &c)); // no source
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\"}]}",
               &c)); // no sinks
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\","
               "\"sinks\":[]}]}",
               &c));
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"usb:a\","
               "\"sinks\":[\"bus:b\"]}]}",
               &c)); // unknown source kind
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"v4l2:\","
               "\"sinks\":[\"bus:b\"]}]}",
               &c)); // no device
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"rpicam:x\","
               "\"sinks\":[\"bus:b\"]}]}",
               &c));
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\","
               "\"sinks\":[\"file:b\"]}]}",
               &c)); // unknown sink kind
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\","
               "\"sinks\":[\"bus:a\"]}]}",
               &c)); // reads its own output
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"v4l2:/dev/v\","
               "\"width\":641,\"sinks\":[\"bus:b\"]}]}",
               &c)); // odd width
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"v4l2:/dev/v\","
               "\"format\":\"yuv420\",\"sinks\":[\"bus:b\"]}]}",
               &c)); // yuv420 is rpicam's
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\","
               "\"sinks\":[\"bus:b\"]},{\"name\":\"p\",\"source\":\"bus:c\","
               "\"sinks\":[\"bus:d\"]}]}",
               &c)); // two pipelines with one name
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\","
               "\"sinks\":[\"bus:1\",\"bus:2\",\"bus:3\",\"bus:4\",\"bus:5\","
               "\"bus:6\",\"bus:7\",\"bus:8\",\"bus:9\"]}]}",
               &c)); // over the sink limit
  char trailing[512];
  snprintf(trailing, sizeof trailing, "%s x", one);
  CHECK(!parse(trailing, &c));
}

// ---- Pipelines, driven through the frame bus ------------------------------

typedef struct {
  uint32_t w, h;
  uint8_t *buf;
  uint8_t *planes[3];
  size_t bytes;
} Img;

static void img_init(Img *im, uint32_t w, uint32_t h) {
  im->w = w;
  im->h = h;
  size_t y = (size_t)w * h, c = (size_t)((w + 1) / 2) * h;
  im->bytes = y + 2 * c;
  im->buf = malloc(im->bytes);
  im->planes[0] = im->buf;
  im->planes[1] = im->buf + y;
  im->planes[2] = im->buf + y + c;
}

static void img_pattern(Img *im, int seed) {
  for (size_t i = 0; i < im->bytes; i++)
    im->buf[i] = (uint8_t)(i * 7 + (size_t)seed * 13);
}


static PipelineConfig bus_pipeline(const char *name, const char *from,
                                   const char *to, const char *extra_sink) {
  char json[512];
  snprintf(json, sizeof json,
           "{\"pipelines\":[{\"name\":\"%s\",\"source\":\"bus:%s\","
           "\"sinks\":[\"bus:%s\"%s%s%s]}]}",
           name, from, to, extra_sink ? ",\"" : "",
           extra_sink ? extra_sink : "", extra_sink ? "\"" : "");
  HellionConfig c;
  bool ok = parse(json, &c);
  CHECK(ok);
  return c.pipelines[0];
}

// Frames go through a pipeline byte for byte, with the publisher's frame
// number and capture time intact.
static void test_pipeline_forwards_frames(void) {
  FbusWriter *in = fbus_writer_open("p-in", 64, 48);
  PipelineConfig pc = bus_pipeline("fwd", "p-in", "p-out", NULL);
  Pipeline *p = pipeline_start(&pc);
  CHECK(p != NULL);

  Img src, dst;
  img_init(&src, 64, 48);
  img_init(&dst, 64, 48);
  img_pattern(&src, 0);
  FbusReader *out = NULL;
  // Publish until the pipeline's output appears (the source has to attach
  // and the sink to start before anything comes out the other side).
  for (int i = 0; i < 100 && !out; i++) {
    fbus_writer_publish(in, (const uint8_t *const *)src.planes, 1000 + i, 0);
    sleep_ms(20);
    out = fbus_reader_open("p-out", NULL);
  }
  CHECK(out != NULL);
  if (!out)
    goto done;

  for (int k = 0; k < 5; k++) {
    img_pattern(&src, k + 1);
    fbus_writer_publish(in, (const uint8_t *const *)src.planes, 5000 + k,
                        123456000u + (uint64_t)k);
    FbusFrameInfo info;
    // Skip anything older that was still in flight.
    bool got = false;
    for (int tries = 0; tries < 20 && !got; tries++) {
      if (fbus_reader_next(out, dst.planes, 1000, &info) != FBUS_FRAME)
        break;
      got = info.frame_no == 5000 + k;
    }
    CHECK(got);
    CHECK_MEM_EQ(dst.buf, src.buf, src.bytes);
    CHECK_EQ_INT(info.capture_mono_ns, 123456000u + (uint64_t)k);
  }

  HsPipeline st;
  pipeline_status(p, &st);
  CHECK(strcmp(st.source.state, "running") == 0);
  CHECK_EQ_INT(st.width, 64);
  CHECK(st.source.frames >= 5);
  CHECK_EQ_INT(st.sink_count, 1);
  CHECK(strcmp(st.sinks[0].state, "publishing") == 0);
  CHECK(st.sinks[0].frames >= 5);

done:
  fbus_reader_close(out);
  pipeline_stop(p);
  fbus_writer_close(in);
  free(src.buf);
  free(dst.buf);
}

// The source restarting at a new size restarts the sinks at that size.
static void test_pipeline_follows_size_change(void) {
  FbusWriter *in = fbus_writer_open("g-in", 32, 16);
  PipelineConfig pc = bus_pipeline("geo", "g-in", "g-out", NULL);
  Pipeline *p = pipeline_start(&pc);
  Img a, b;
  img_init(&a, 32, 16);
  img_init(&b, 80, 40);
  img_pattern(&a, 1);
  img_pattern(&b, 2);

  FbusStatus st = {0};
  for (int i = 0; i < 100; i++) {
    fbus_writer_publish(in, (const uint8_t *const *)a.planes, i, 0);
    sleep_ms(20);
    if (fbus_status("g-out", &st) == FBUS_OPEN_OK && st.frames > 0)
      break;
  }
  CHECK_EQ_INT(st.width, 32);

  fbus_writer_close(in);
  in = fbus_writer_open("g-in", 80, 40);
  memset(&st, 0, sizeof st);
  for (int i = 0; i < 150; i++) {
    fbus_writer_publish(in, (const uint8_t *const *)b.planes, i, 0);
    sleep_ms(20);
    if (fbus_status("g-out", &st) == FBUS_OPEN_OK && st.width == 80 &&
        st.frames > 0)
      break;
  }
  CHECK_EQ_INT(st.width, 80);
  CHECK_EQ_INT(st.height, 40);

  pipeline_stop(p);
  fbus_writer_close(in);
  free(a.buf);
  free(b.buf);
}

// A sink that cannot deliver (here, a "v4l2 device" that is /dev/null)
// must not cost its neighbour any frames: each sink has its own thread.
static void test_failing_sink_does_not_starve_others(void) {
  FbusWriter *in = fbus_writer_open("f-in", 32, 16);
  PipelineConfig pc = bus_pipeline("fail", "f-in", "f-out", "v4l2:/dev/null");
  Pipeline *p = pipeline_start(&pc);
  Img a;
  img_init(&a, 32, 16);
  img_pattern(&a, 3);

  FbusReader *out = NULL;
  for (int i = 0; i < 100 && !out; i++) {
    fbus_writer_publish(in, (const uint8_t *const *)a.planes, i, 0);
    sleep_ms(20);
    out = fbus_reader_open("f-out", NULL);
  }
  CHECK(out != NULL);
  int got = 0;
  for (int i = 0; i < 30; i++) {
    fbus_writer_publish(in, (const uint8_t *const *)a.planes, 100 + i, 0);
    sleep_ms(10);
    if (out && fbus_reader_next(out, a.planes, 200, NULL) == FBUS_FRAME)
      got++;
  }
  CHECK(got >= 25);

  HsPipeline st;
  pipeline_status(p, &st);
  CHECK_EQ_INT(st.sink_count, 2);
  CHECK(strcmp(st.sinks[0].state, "publishing") == 0);
  CHECK(strcmp(st.sinks[1].state, "waiting") == 0);
  CHECK_EQ_INT(st.sinks[1].frames, 0);

  fbus_reader_close(out);
  pipeline_stop(p);
  fbus_writer_close(in);
  free(a.buf);
}

static void test_pipeline_waits_for_missing_source(void) {
  PipelineConfig pc = bus_pipeline("miss", "never-there", "m-out", NULL);
  Pipeline *p = pipeline_start(&pc);
  sleep_ms(200);
  HsPipeline st;
  pipeline_status(p, &st);
  CHECK(strcmp(st.source.state, "waiting") == 0);
  CHECK(strstr(st.source.detail, "no such stream") != NULL);
  CHECK(strcmp(st.sinks[0].state, "waiting") == 0); // no geometry yet
  pipeline_stop(p); // must not hang
}

// The shipped examples are what install.sh starts from: they must load.
// (make check runs suites from the repository root.)
static void test_example_configs_load(void) {
  static const char *const files[] = {
      "hellion/examples/a-rov.json", "hellion/examples/a-topside.json",
      "hellion/examples/b-rov.json", "hellion/examples/b-topside.json"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    HellionConfig c;
    bool ok = hconfig_load(files[i], &c);
    if (!ok)
      printf("    %s did not load\n", files[i]);
    CHECK(ok);
  }
}

// ---- RTP config -------------------------------------------------------------

// Placement A's camera controls: forwarded to the peer, so a peer is
// required; kept in camera_file, which must be absolute.
static void test_config_camera(void) {
  HellionConfig c;
  const char *pipe = "\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\","
                     "\"sinks\":[\"bus:b\"]}]";
  char json[512];
  snprintf(json, sizeof json, "{%s}", pipe);
  CHECK(parse(json, &c));
  CHECK(c.camera_controls[0] == '\0');
  CHECK(strcmp(c.camera_file, "/var/lib/hellion/camera.json") == 0);
  snprintf(json, sizeof json,
           "{\"peer\":\"192.168.2.2:8091\",\"camera_controls\":\"peer\","
           "\"camera_file\":\"/tmp/c.json\",%s}", pipe);
  CHECK(parse(json, &c));
  CHECK(strcmp(c.camera_controls, "peer") == 0);
  CHECK(strcmp(c.camera_file, "/tmp/c.json") == 0);
  snprintf(json, sizeof json, "{\"camera_controls\":\"peer\",%s}", pipe);
  CHECK(!parse(json, &c)); // no peer to send them to
  snprintf(json, sizeof json, "{\"camera_controls\":\"here\",%s}", pipe);
  CHECK(!parse(json, &c));
  snprintf(json, sizeof json, "{\"camera_file\":\"camera.json\",%s}", pipe);
  CHECK(!parse(json, &c));
}

static void test_config_rtp(void) {
  HellionConfig c;
  CHECK(parse("{\"pipelines\":[{\"name\":\"down\",\"source\":\"rtp:5610\","
              "\"width\":640,\"height\":480,"
              "\"relay\":[\"127.0.0.1:5600\",\"[fe80::1%eth0]:5602\"],"
              "\"sinks\":[\"bus:pilot\"]},"
              "{\"name\":\"up\",\"source\":\"bus:pilot\","
              "\"encoder\":\"h264_v4l2m2m\",\"bitrate_kbps\":2500,"
              "\"gop\":15,\"framerate\":25,"
              "\"sinks\":[\"rtp:192.168.2.1:5610\"]}]}",
              &c));
  CHECK_EQ_INT(c.pipelines[0].source, SRC_RTP);
  CHECK(strcmp(c.pipelines[0].source_arg, "5610") == 0);
  CHECK_EQ_INT(c.pipelines[0].relay_count, 2);
  CHECK(strcmp(c.pipelines[0].relays[1], "[fe80::1%eth0]:5602") == 0);
  CHECK_EQ_INT(c.pipelines[1].sinks[0].kind, SINK_RTP);
  CHECK(strcmp(c.pipelines[1].sinks[0].arg, "192.168.2.1:5610") == 0);
  CHECK(strcmp(c.pipelines[1].encoder, "h264_v4l2m2m") == 0);
  CHECK_EQ_INT(c.pipelines[1].bitrate_kbps, 2500);
  CHECK_EQ_INT(c.pipelines[1].gop, 15);
  CHECK_EQ_INT(c.pipelines[1].framerate, 25);

  // Defaults.
  CHECK(parse("{\"pipelines\":[{\"name\":\"u\",\"source\":\"bus:a\","
              "\"sinks\":[\"rtp:h:1\"]}]}",
              &c));
  CHECK(strcmp(c.pipelines[0].encoder, "libx264") == 0);
  CHECK_EQ_INT(c.pipelines[0].bitrate_kbps, 4000);
  CHECK_EQ_INT(c.pipelines[0].gop, 30);
  CHECK_EQ_INT(c.pipelines[0].framerate, 30);

  // Errors.
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"rtp:0\","
               "\"sinks\":[\"bus:b\"]}]}",
               &c)); // port 0
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"rtp:70000\","
               "\"sinks\":[\"bus:b\"]}]}",
               &c));
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\","
               "\"sinks\":[\"rtp:5600\"]}]}",
               &c)); // a sink needs a host
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\","
               "\"relay\":[\"h:1\"],\"sinks\":[\"bus:b\"]}]}",
               &c)); // relay only for an rtp source
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"rtp:1\","
               "\"relay\":[\"nohostport\"],\"sinks\":[\"bus:b\"]}]}",
               &c));
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\","
               "\"encoder\":\"x264; rm -rf\",\"sinks\":[\"bus:b\"]}]}",
               &c));
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\","
               "\"bitrate_kbps\":10,\"sinks\":[\"bus:b\"]}]}",
               &c));
  CHECK(!parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\","
               "\"framerate\":0,\"sinks\":[\"bus:b\"]}]}",
               &c));
}

// ---- UDP relay ----------------------------------------------------------------

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

static int udp_listener(int *port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind(fd, (struct sockaddr *)&a, sizeof a);
  socklen_t l = sizeof a;
  getsockname(fd, (struct sockaddr *)&a, &l);
  *port = ntohs(a.sin_port);
  struct timeval tv = {0, 300000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return fd;
}

static void send_rtp(int fd, int port, uint16_t seq, uint32_t ssrc,
                     size_t len) {
  uint8_t pkt[1500];
  memset(pkt, 0xAB, sizeof pkt);
  pkt[0] = 0x80; // version 2
  pkt[1] = 96;
  pkt[2] = (uint8_t)(seq >> 8);
  pkt[3] = (uint8_t)seq;
  pkt[8] = (uint8_t)(ssrc >> 24);
  pkt[9] = (uint8_t)(ssrc >> 16);
  pkt[10] = (uint8_t)(ssrc >> 8);
  pkt[11] = (uint8_t)ssrc;
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons((uint16_t)port);
  sendto(fd, pkt, len, 0, (struct sockaddr *)&a, sizeof a);
}

static int drain(int fd) {
  int n = 0;
  uint8_t buf[2048];
  while (recv(fd, buf, sizeof buf, 0) > 0)
    n++;
  return n;
}

static void send_rtp_marked(int fd, int port, uint16_t seq, bool marker) {
  uint8_t pkt[100];
  memset(pkt, 0xAB, sizeof pkt);
  pkt[0] = 0x80;
  pkt[1] = (uint8_t)(96 | (marker ? 0x80 : 0));
  pkt[2] = (uint8_t)(seq >> 8);
  pkt[3] = (uint8_t)seq;
  memset(pkt + 4, 0x11, 4); // timestamp
  memset(pkt + 8, 0x22, 4); // SSRC
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons((uint16_t)port);
  sendto(fd, pkt, sizeof pkt, 0, (struct sockaddr *)&a, sizeof a);
}

// The decoder destination gets an access unit delimiter after each
// frame's last packet, with the sequence numbers shifted so the stream it
// sees has no gaps; an ordinary destination gets the packets unchanged.
static void test_udp_relay_frame_ends(void) {
  char err[160];
  int pd, pq;
  int dec = udp_listener(&pd), qgc = udp_listener(&pq);
  UdpRelay *r = udp_relay_open("127.0.0.1:0", err, sizeof err);
  CHECK(r != NULL);
  if (!r)
    return;
  char dest[64];
  snprintf(dest, sizeof dest, "127.0.0.1:%d", pd);
  CHECK(udp_relay_add_decoder(r, dest, err, sizeof err));
  snprintf(dest, sizeof dest, "127.0.0.1:%d", pq);
  CHECK(udp_relay_add_dest(r, dest, err, sizeof err));
  CHECK(udp_relay_start(r));
  int in = socket(AF_INET, SOCK_DGRAM, 0);
  int port = udp_relay_port(r);
  // Two frames of two packets, then one of one; seq wraps in the middle.
  uint16_t seqs[] = {65534, 65535, 0, 1, 2};
  bool marks[] = {false, true, false, true, true};
  for (int i = 0; i < 5; i++)
    send_rtp_marked(in, port, seqs[i], marks[i]);
  sleep_ms(200);

  // Decoder: 5 packets + 3 AUDs, consecutive sequence numbers from 65534.
  uint8_t buf[2048];
  uint16_t want = 65534;
  int got = 0, auds = 0;
  ssize_t n;
  while ((n = recv(dec, buf, sizeof buf, 0)) > 0) {
    uint16_t seq = (uint16_t)((buf[2] << 8) | buf[3]);
    CHECK_EQ_INT(seq, want);
    want++;
    got++;
    if (n == 14) {
      auds++;
      CHECK(buf[0] == 0x80 && (buf[1] & 0x80) == 0 && (buf[1] & 0x7f) == 96);
      CHECK(buf[12] == 0x09 && buf[13] == 0xf0);
      CHECK(buf[4] == 0x11 && buf[8] == 0x22); // the frame's timestamp, SSRC
    }
  }
  CHECK_EQ_INT(got, 8);
  CHECK_EQ_INT(auds, 3);

  // QGC: exactly what was sent.
  int i = 0;
  while ((n = recv(qgc, buf, sizeof buf, 0)) > 0) {
    CHECK_EQ_INT(n, 100);
    if (i < 5)
      CHECK_EQ_INT((buf[2] << 8) | buf[3], seqs[i]);
    i++;
  }
  CHECK_EQ_INT(i, 5);

  // Loss is measured on the stream as received, not the decoder's copy.
  RelayStats st;
  udp_relay_stats(r, &st);
  CHECK_EQ_INT(st.lost, 0);
  CHECK_EQ_INT(st.packets, 5);
  udp_relay_close(r);
  close(in);
  close(dec);
  close(qgc);
}

// Every packet reaches every destination unchanged, and the RTP stats see
// through to what the link did: gaps are loss, late arrivals are not, and
// a new sender (SSRC) is not a gap.
static void test_udp_relay(void) {
  char err[160];
  int pa, pb;
  int a = udp_listener(&pa), b = udp_listener(&pb);
  UdpRelay *r = udp_relay_open("127.0.0.1:0", err, sizeof err);
  CHECK(r != NULL);
  char dest[64];
  snprintf(dest, sizeof dest, "127.0.0.1:%d", pa);
  CHECK(udp_relay_add_dest(r, dest, err, sizeof err));
  snprintf(dest, sizeof dest, "127.0.0.1:%d", pb);
  CHECK(udp_relay_add_dest(r, dest, err, sizeof err));
  CHECK(!udp_relay_add_dest(r, "no-port", err, sizeof err));
  CHECK(udp_relay_start(r));
  int in = socket(AF_INET, SOCK_DGRAM, 0);
  int port = udp_relay_port(r);
  CHECK(port > 0);

  // 1..10, skipping 4 and 5 (lost), then 4 arrives late, then 11..12.
  for (uint16_t s = 1; s <= 10; s++)
    if (s != 4 && s != 5)
      send_rtp(in, port, s, 0x1111, 1200);
  send_rtp(in, port, 4, 0x1111, 1200);
  send_rtp(in, port, 11, 0x1111, 1200);
  send_rtp(in, port, 12, 0x1111, 1200);
  // A new sender, starting somewhere else entirely: no false gap.
  send_rtp(in, port, 50000, 0x2222, 1200);
  send_rtp(in, port, 50001, 0x2222, 1200);
  // Not RTP at all: forwarded, counted, but no sequence bookkeeping.
  send_rtp(in, port, 0, 0, 4);
  sleep_ms(200);

  CHECK_EQ_INT(drain(a), 14);
  CHECK_EQ_INT(drain(b), 14);
  RelayStats st;
  udp_relay_stats(r, &st);
  CHECK_EQ_INT(st.packets, 14);
  CHECK_EQ_INT(st.lost, 1); // 5; 4 turned up late
  CHECK_EQ_INT(st.late, 1);
  CHECK(st.idle_s >= 0 && st.idle_s < 1.0);

  // Wrap-around is not loss.
  send_rtp(in, port, 65535, 0x3333, 100);
  send_rtp(in, port, 0, 0x3333, 100);
  send_rtp(in, port, 1, 0x3333, 100);
  sleep_ms(100);
  udp_relay_stats(r, &st);
  CHECK_EQ_INT(st.lost, 1);

  // A long outage with the sender still going (cable pulled): it comes
  // back 40000 numbers on, which modulo 2^16 looks like "behind". After a
  // second of silence that is a fresh start, and loss after it is counted
  // again -- it used to read as late packets, hiding real loss for up to a
  // minute.
  sleep_ms(1200);
  send_rtp(in, port, 40001, 0x3333, 100);
  send_rtp(in, port, 40002, 0x3333, 100);
  send_rtp(in, port, 40004, 0x3333, 100); // 40003 lost
  sleep_ms(100);
  udp_relay_stats(r, &st);
  CHECK_EQ_INT(st.lost, 2);
  CHECK_EQ_INT(st.late, 1);

  udp_relay_close(r);
  close(in);
  close(a);
  close(b);

  UdpRelay *bad = udp_relay_open("not-an-address:x", err, sizeof err);
  CHECK(bad == NULL);
}

// ---- RTP end to end ---------------------------------------------------------

static double mean_y(const Img *im) {
  unsigned long sum = 0;
  size_t n = (size_t)im->w * im->h;
  for (size_t i = 0; i < n; i++)
    sum += im->planes[0][i];
  return (double)sum / (double)n;
}

// Bus -> encoder -> RTP -> relay -> decoder -> bus, through two pipelines
// as on the ROV and topside. Uniform dark and bright frames must come
// back at the brightness they went in: that only holds if the full->
// limited range conversion on the way in and limited->full on the way out
// are both right (wrong, and dark grey turns black or white turns grey).
static void test_rtp_round_trip(void) {
  if (!ff_available()) {
    printf("    (skipped: ffmpeg is not installed)\n");
    return;
  }
  int port = udp_free_port_pair();
  char json[512];
  snprintf(json, sizeof json,
           "{\"pipelines\":[{\"name\":\"down\",\"source\":\"rtp:%d\","
           "\"width\":320,\"height\":240,\"sinks\":[\"bus:rt-out\"]},"
           "{\"name\":\"up\",\"source\":\"bus:rt-in\",\"gop\":10,"
           "\"sinks\":[\"rtp:127.0.0.1:%d\"]}]}",
           port, port);
  HellionConfig c;
  CHECK(parse(json, &c));
  FbusWriter *in = fbus_writer_open("rt-in", 320, 240);
  Pipeline *down = pipeline_start(&c.pipelines[0]);
  Pipeline *up = pipeline_start(&c.pipelines[1]);

  Img src, dst;
  img_init(&src, 320, 240);
  img_init(&dst, 320, 240);
  memset(src.planes[1], 128, src.bytes - (size_t)320 * 240); // neutral chroma

  static const uint8_t levels[] = {40, 200};
  FbusReader *out = NULL;
  for (int li = 0; li < 2; li++) {
    memset(src.planes[0], levels[li], (size_t)320 * 240);
    double got = -1;
    // Publish at 30fps until the decoded output has settled on this level
    // (the first frames after a change can be mid-transition).
    for (int i = 0; i < 300; i++) {
      fbus_writer_publish(in, (const uint8_t *const *)src.planes, i, 0);
      sleep_ms(33);
      if (!out)
        out = fbus_reader_open("rt-out", NULL);
      if (out && fbus_reader_next(out, dst.planes, 10, NULL) == FBUS_FRAME) {
        got = mean_y(&dst);
        if (got > levels[li] - 4 && got < levels[li] + 4 && i > 60)
          break;
      }
    }
    printf("    Y %u in, %.1f out\n", levels[li], got);
    CHECK(got > levels[li] - 4 && got < levels[li] + 4);
  }

  HsPipeline st;
  pipeline_status(up, &st);
  CHECK(strcmp(st.sinks[0].state, "sending") == 0);
  CHECK(st.sinks[0].net && st.sinks[0].packets > 0);
  pipeline_status(down, &st);
  CHECK(st.source.net && st.source.packets > 0);
  CHECK_EQ_INT(st.source.lost, 0);

  fbus_reader_close(out);
  pipeline_stop(up);
  pipeline_stop(down);
  fbus_writer_close(in);
  free(src.buf);
  free(dst.buf);
}

// True if the file contains `tag` (an MP4 box type) anywhere.
static bool file_has(const char *path, const char *tag) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return false;
  static char buf[1 << 20];
  size_t n = fread(buf, 1, sizeof buf, f);
  fclose(f);
  for (size_t i = 0; i + 4 <= n; i++)
    if (memcmp(buf + i, tag, 4) == 0)
      return true;
  return false;
}

// The camera keeps running while a recording stops: hellion forwards packets
// to the recording ffmpeg until it exits, because ffmpeg (6.1, as Ubuntu 24.04
// ships it) acts on SIGINT only when its input read returns. A test that
// stopped publishing first would leave that read waiting out the grace period
// and see the recording killed instead of stopped (newer ffmpeg does not wait).
typedef struct {
  FbusWriter *w;
  Img *src;
  int seq;
  atomic_bool run;
} Feeder;

static void *feeder_loop(void *arg) {
  Feeder *fd = (Feeder *)arg;
  while (atomic_load(&fd->run)) {
    img_pattern(fd->src, fd->seq);
    fbus_writer_publish(fd->w, (const uint8_t *const *)fd->src->planes, fd->seq, 0);
    fd->seq++;
    sleep_ms(33);
  }
  return NULL;
}

// Recording the RTP arriving at a pipeline: a fragmented MP4 (ftyp, then
// moof fragments, which is what makes a cut-off recording still play),
// one at a time, and stop refusing when nothing is recording.
static void test_recorder(void) {
  if (!ff_available()) {
    printf("    (skipped: ffmpeg is not installed)\n");
    return;
  }
  int port = udp_free_port_pair();
  char json[1024];
  snprintf(json, sizeof json,
           "{\"record_dir\":\"%s/rec\",\"pipelines\":["
           "{\"name\":\"down\",\"source\":\"rtp:%d\",\"width\":320,"
           "\"height\":240,\"sinks\":[\"bus:rec-out\"]},"
           "{\"name\":\"up\",\"source\":\"bus:rec-in\",\"gop\":10,"
           "\"sinks\":[\"rtp:127.0.0.1:%d\"]}]}",
           bus_dir, port, port);
  HellionConfig c;
  CHECK(parse(json, &c));
  FbusWriter *in = fbus_writer_open("rec-in", 320, 240);
  Pipeline *pipes[2] = {pipeline_start(&c.pipelines[0]),
                        pipeline_start(&c.pipelines[1])};
  HRec *rec = hrec_create(&c, pipes, 2);

  Img src;
  img_init(&src, 320, 240);
  char detail[256];
  bool started = false;
  // Publish until the receiving side is up, then record ~3s.
  for (int i = 0; i < 300; i++) {
    img_pattern(&src, i);
    fbus_writer_publish(in, (const uint8_t *const *)src.planes, i, 0);
    sleep_ms(33);
    if (!started && i > 30 && pipeline_rtp_relay(pipes[0])) {
      started = hrec_start(rec, detail, sizeof detail);
      CHECK(started);
      CHECK(!hrec_start(rec, detail, sizeof detail)); // one at a time
      CHECK(strstr(detail, "already recording") != NULL);
    }
    if (started && i > 120)
      break;
  }
  char status[1200];
  hrec_status_json(rec, status, sizeof status);
  CHECK(strstr(status, "\"recording\":true") != NULL);

  Feeder feed = {.w = in, .src = &src, .seq = 1000};
  atomic_init(&feed.run, true);
  pthread_t feeder;
  CHECK(pthread_create(&feeder, NULL, feeder_loop, &feed) == 0);
  CHECK(hrec_stop(rec, detail, sizeof detail));
  atomic_store(&feed.run, false);
  pthread_join(feeder, NULL);
  char path[512];
  snprintf(path, sizeof path, "%s/rec/%s", bus_dir, detail);
  struct stat st;
  CHECK(stat(path, &st) == 0 && st.st_size > 10000);
  CHECK(file_has(path, "ftyp"));
  CHECK(file_has(path, "moof")); // fragmented
  hrec_status_json(rec, status, sizeof status);
  CHECK(strstr(status, "\"recording\":false") != NULL);
  CHECK(strstr(status, "\"error\":\"\"") != NULL); // stopped cleanly
  if (!strstr(status, "\"error\":\"\""))
    printf("    recorder status: %s\n", status);
  CHECK(!hrec_stop(rec, detail, sizeof detail));
  unlink(path);
  char dir[300];
  snprintf(dir, sizeof dir, "%s/rec", bus_dir);
  rmdir(dir);

  hrec_destroy(rec);
  pipeline_stop(pipes[1]);
  pipeline_stop(pipes[0]);
  fbus_writer_close(in);
  free(src.buf);

  // Nothing to record: unavailable, with the reason.
  CHECK(parse("{\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\","
              "\"sinks\":[\"bus:b\"]}]}",
              &c));
  rec = hrec_create(&c, NULL, 0);
  hrec_status_json(rec, status, sizeof status);
  CHECK(strstr(status, "\"available\":false") != NULL);
  CHECK(!hrec_start(rec, detail, sizeof detail));
  CHECK(strstr(detail, "no RTP source") != NULL);
  hrec_destroy(rec);
}

// ---- Status file ----------------------------------------------------------

static void test_status_round_trip(void) {
  HsWriter *w = hs_writer_open("st-test");
  CHECK(w != NULL);
  static HsStatus snap, back;
  memset(&snap, 0, sizeof snap);
  snprintf(snap.name, sizeof snap.name, "st-test");
  snprintf(snap.role, sizeof snap.role, "topside");
  snap.pipeline_count = 1;
  snprintf(snap.pipelines[0].name, hs_name_len, "pipe");
  snap.pipelines[0].sink_count = 1;
  snprintf(snap.pipelines[0].sinks[0].state, hs_state_len, "writing");
  snap.pipelines[0].sinks[0].frames = 42;
  hs_writer_publish(w, &snap);

  CHECK_EQ_INT(hs_read("st-test", &back), HS_READ_OK);
  CHECK(strcmp(back.role, "topside") == 0);
  CHECK_EQ_INT(back.pid, getpid());
  CHECK_EQ_INT(back.pipeline_count, 1);
  CHECK_EQ_INT(back.pipelines[0].sinks[0].frames, 42);
  CHECK(strcmp(back.pipelines[0].sinks[0].state, "writing") == 0);
  CHECK(hs_heartbeat_age(&back) < 1.0);

  char names[8][hs_name_len];
  int n = hs_list(names, 8);
  bool found = false;
  for (int i = 0; i < n; i++)
    found |= strcmp(names[i], "st-test") == 0;
  CHECK(found);

  hs_writer_close(w);
  CHECK_EQ_INT(hs_read("st-test", &back), HS_READ_MISSING);

  // A file of the wrong size (another build of hellion) is refused.
  char path[256];
  snprintf(path, sizeof path, "%s/hellion.st-odd", bus_dir);
  FILE *f = fopen(path, "wb");
  fputs("not a status file", f);
  fclose(f);
  CHECK_EQ_INT(hs_read("st-odd", &back), HS_READ_MISMATCH);
}

// ---- Log capture ----------------------------------------------------------

static bool log_has(const char *needle) {
  static char lines[hlog_lines][hlog_line_len];
  size_t n = hlog_snapshot(lines, hlog_lines, NULL);
  for (size_t i = 0; i < n; i++)
    if (strstr(lines[i], needle))
      return true;
  return false;
}

// Own output and a child process's stderr both land in the ring, and
// stdout is put back afterwards.
static void test_log_capture(void) {
  CHECK(hlog_start());
  printf("hlog test line one\n");
  fprintf(stderr, "hlog test on stderr\n");
  ChildProc c;
  char *const argv[] = {"sh", "-c", "echo hlog test from a child >&2", NULL};
  if (child_spawn(&c, "test", argv, CHILD_NULL_STDIN)) {
    child_wait(&c, 2000, NULL);
    child_stop(&c, 100);
  }
  fflush(stdout);
  sleep_ms(100); // the reader thread runs asynchronously
  bool own = log_has("hlog test line one");
  bool err = log_has("hlog test on stderr");
  bool child = log_has("hlog test from a child");
  hlog_stop();
  CHECK(own);
  CHECK(err);
  CHECK(child);
  uint64_t total;
  static char lines[hlog_lines][hlog_line_len];
  hlog_snapshot(lines, 1, &total);
  CHECK(total >= 3);
  // Timestamped, as top shows them.
  CHECK(lines[0][2] == ':' && lines[0][5] == ':');
}

static void remove_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  char path[512];
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.' && (!e->d_name[1] || e->d_name[1] == '.'))
      continue;
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    unlink(path);
  }
  closedir(d);
  rmdir(dir);
}

// ---- Discovery ----------------------------------------------------------------

static Beacon beacon(const char *role, const char *name, const char *pair,
                     char placement, uint32_t id) {
  Beacon b;
  memset(&b, 0, sizeof b);
  snprintf(b.role, sizeof b.role, "%s", role);
  snprintf(b.name, sizeof b.name, "%s", name);
  snprintf(b.pair, sizeof b.pair, "%s", pair);
  b.placement = placement;
  b.control = 8091;
  b.tether = 0;
  b.id = id;
  return b;
}

// A beacon reads back as written; anything that is not a well-formed
// version-1 beacon is refused, and keys from a later version are ignored.
static void test_discovery_beacons(void) {
  Beacon a = beacon("rov", "rov-1", "dive_team", 'b', 0x1a2b3c4d), b;
  char text[300];
  CHECK(disc_beacon_format(&a, text, sizeof text));
  CHECK(strcmp(text, "HELLION/1 role=rov name=rov-1 pair=dive_team "
                     "placement=b control=8091 tether=0 id=1a2b3c4d") == 0);
  CHECK(disc_beacon_parse(text, strlen(text), &b));
  CHECK(strcmp(b.role, "rov") == 0 && strcmp(b.name, "rov-1") == 0);
  CHECK(strcmp(b.pair, "dive_team") == 0 && b.placement == 'b');
  CHECK(b.control == 8091 && b.tether == 0 && b.id == 0x1a2b3c4d);

  const char *ok[] = {
      "HELLION/1 role=topside pair=default id=00000001\n", // minimal, newline
      "HELLION/1 role=rov pair=p id=ffffffff later=1 x=y", // unknown keys
  };
  for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
    CHECK(disc_beacon_parse(ok[i], strlen(ok[i]), &b));
  CHECK(b.placement == '?' && b.name[0] == '\0' && b.id == 0xffffffffu);

  const char *bad[] = {
      "",
      "HELLION/2 role=rov pair=p id=00000001",      // a different version
      "HELLION/10 role=rov pair=p id=00000001",
      "HELLO role=rov pair=p id=00000001",
      "HELLION/1 pair=p id=00000001",               // no role
      "HELLION/1 role=rov id=00000001",             // no pair
      "HELLION/1 role=rov pair=p",                  // no id
      "HELLION/1 role=boat pair=p id=00000001",     // bad role
      "HELLION/1 role=rov pair=p id=123",           // id not 8 hex digits
      "HELLION/1 role=rov pair=p id=0000000g",
      "HELLION/1 role=rov pair=a.b id=00000001",    // bad name characters
      "HELLION/1 role=rov pair=p control=70000 id=00000001",
      "HELLION/1 role=rov pair=p placement=c id=00000001",
      "HELLION/1 role=rov pair=p\x01 id=00000001",  // control byte
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    bool parsed = disc_beacon_parse(bad[i], strlen(bad[i]), &b);
    if (parsed)
      printf("    accepted: \"%s\"\n", bad[i]);
    CHECK(!parsed);
  }
  // Oversize: a valid beacon padded past disc_beacon_max.
  char big[400];
  memset(big, 'x', sizeof big);
  memcpy(big, "HELLION/1 role=rov pair=p id=00000001 pad=", 42);
  big[sizeof big - 1] = '\0';
  CHECK(!disc_beacon_parse(big, strlen(big), &b));
}

static Candidate cand(Beacon b, const char *v4, uint64_t seen4,
                      const char *v6, uint64_t seen6) {
  Candidate c;
  memset(&c, 0, sizeof c);
  c.b = b;
  snprintf(c.addr4, sizeof c.addr4, "%s", v4);
  snprintf(c.addr6, sizeof c.addr6, "%s", v6);
  c.seen4_ns = seen4;
  c.seen6_ns = seen6;
  c.seen_ns = seen4 > seen6 ? seen4 : seen6;
  return c;
}

// Choosing: the one matching hellion; never a guess between two; nothing
// stale, of our own role, another pair or another placement.
static void test_discovery_choice(void) {
  const uint64_t s = 1000000000ull, now = 100 * s;
  DiscWant rov = {"rov", "default", 'b'};
  Beacon top1 = beacon("topside", "top-1", "default", 'b', 1);
  Beacon top2 = beacon("topside", "top-2", "default", '?', 2);
  DiscView v;

  Candidate one[] = {cand(top1, "192.168.2.1", now - s, "", 0)};
  memset(&v, 0, sizeof v);
  disc_choose(one, 1, &rov, now, &v);
  CHECK_EQ_INT(v.state, DISC_FOUND);
  CHECK(strcmp(v.addr, "192.168.2.1") == 0 && v.peer.id == 1);

  // IPv4 preferred while it is fresh; IPv6 link-local once it is not.
  Candidate both[] = {cand(top1, "192.168.2.1", now - s, "fe80::1%eth0",
                           now - s)};
  disc_choose(both, 1, &rov, now, &v);
  CHECK(strcmp(v.addr, "192.168.2.1") == 0);
  both[0].seen4_ns = now - 9 * s;
  disc_choose(both, 1, &rov, now, &v);
  CHECK(strcmp(v.addr, "fe80::1%eth0") == 0);

  // Two answering the same pair: refused, and the status says what to do.
  Candidate two[] = {cand(top1, "192.168.1.10", now - s, "", 0),
                     cand(top2, "192.168.1.11", now - s, "", 0)};
  disc_choose(two, 2, &rov, now, &v);
  CHECK_EQ_INT(v.state, DISC_AMBIGUOUS);
  CHECK(strstr(v.detail, "2 topsides answer pair \"default\"") != NULL);
  CHECK(strstr(v.detail, "192.168.1.10") && strstr(v.detail, "192.168.1.11"));
  CHECK(strstr(v.detail, "--pair") != NULL);
  // ...unless one of them has gone quiet for longer than the timeout.
  two[1].seen4_ns = two[1].seen_ns = now - 6 * s;
  disc_choose(two, 2, &rov, now, &v);
  CHECK_EQ_INT(v.state, DISC_FOUND);
  CHECK(v.peer.id == 1);

  // Stale, own role, other pair, other placement: none chosen, and why.
  Candidate none[] = {
      cand(top1, "10.0.0.1", now - 6 * s, "", 0),
      cand(beacon("rov", "rov-2", "default", 'b', 3), "10.0.0.2", now, "", 0),
      cand(beacon("topside", "t", "other", 'b', 4), "10.0.0.3", now, "", 0),
      cand(beacon("topside", "t", "default", 'a', 5), "10.0.0.4", now, "", 0),
  };
  disc_choose(none, 4, &rov, now, &v);
  CHECK_EQ_INT(v.state, DISC_SEARCHING);
  CHECK(strstr(v.detail, "10.0.0.3 has pair \"other\"") != NULL);
  CHECK(strstr(v.detail, "10.0.0.4 is placement a") != NULL);
  CHECK(strstr(v.detail, "10.0.0.1") == NULL); // stale: not even listed

  disc_choose(NULL, 0, &rov, now, &v);
  CHECK_EQ_INT(v.state, DISC_SEARCHING);
  CHECK(strstr(v.detail, "no topside heard") != NULL);

  // Unknown placement on either side does not refuse.
  DiscWant anyp = {"rov", "default", '?'};
  Candidate a_top[] = {cand(beacon("topside", "t", "default", 'a', 6),
                            "10.0.0.5", now, "", 0)};
  disc_choose(a_top, 1, &anyp, now, &v);
  CHECK_EQ_INT(v.state, DISC_FOUND);
}

static void test_config_discovery(void) {
  HellionConfig c;
  const char *pipe = "\"pipelines\":[{\"name\":\"p\",\"source\":\"bus:a\","
                     "\"sinks\":[\"rtp:auto:5610\"]}]";
  char json[512];
  snprintf(json, sizeof json, "{%s}", pipe);
  CHECK(parse(json, &c));
  CHECK(c.discovery && strcmp(c.pair, "default") == 0 && !c.placement[0]);
  CHECK(hcfg_sink_is_auto(&c.pipelines[0].sinks[0]));
  CHECK(!hcfg_peer_is_auto(&c));
  snprintf(json, sizeof json,
           "{\"peer\":\"auto\",\"pair\":\"team-2\",\"placement\":\"b\",%s}",
           pipe);
  CHECK(parse(json, &c));
  CHECK(hcfg_peer_is_auto(&c) && strcmp(c.pair, "team-2") == 0);
  CHECK(strcmp(c.placement, "b") == 0);
  snprintf(json, sizeof json, "{\"discovery\":false,%s}", pipe);
  CHECK(!parse(json, &c)); // an auto sink with nothing to find it
  snprintf(json, sizeof json, "{\"pair\":\"a b\",%s}", pipe);
  CHECK(!parse(json, &c));
  snprintf(json, sizeof json, "{\"placement\":\"c\",%s}", pipe);
  CHECK(!parse(json, &c));
  snprintf(json, sizeof json, "{\"peer\":\"automatic\",%s}", pipe);
  CHECK(!parse(json, &c));
}

// Two processes find each other: a "topside" child and this process as
// the "rov", each sending its beacons to the other's port (broadcast does
// not work on loopback, hence the test-only unicast override). The rov
// picks the topside, then drops it once it has been silent 5s.
static void test_discovery_two_processes(void) {
  int pa = 0, pb = 0;
  int fa = udp_listener(&pa), fb = udp_listener(&pb); // two free ports
  close(fa);
  close(fb);
  char to[64];
  pid_t child = fork();
  if (child == 0) {
    snprintf(to, sizeof to, "%d", pa);
    setenv("HELLION_DISCOVERY_PORT", to, 1);
    snprintf(to, sizeof to, "127.0.0.1:%d", pb);
    setenv("HELLION_DISCOVERY_TO", to, 1);
    DiscConfig dc;
    memset(&dc, 0, sizeof dc);
    dc.self = beacon("topside", "test-top", "default", 'b', 0);
    dc.self.control = 18080;
    dc.self.tether = 18610;
    if (!disc_start(&dc))
      _exit(1);
    for (;;)
      pause();
  }
  snprintf(to, sizeof to, "%d", pb);
  setenv("HELLION_DISCOVERY_PORT", to, 1);
  snprintf(to, sizeof to, "127.0.0.1:%d", pa);
  setenv("HELLION_DISCOVERY_TO", to, 1);
  DiscConfig dc;
  memset(&dc, 0, sizeof dc);
  dc.self = beacon("rov", "test-rov", "default", 'b', 0);
  CHECK(disc_start(&dc));

  DiscView v;
  memset(&v, 0, sizeof v);
  for (int i = 0; i < 40 && v.state != DISC_FOUND; i++) {
    sleep_ms(100);
    disc_view(&v);
  }
  CHECK_EQ_INT(v.state, DISC_FOUND);
  CHECK(strcmp(v.addr, "127.0.0.1") == 0);
  CHECK(strcmp(v.peer.name, "test-top") == 0 && v.peer.tether == 18610);
  char hp[128];
  CHECK(disc_peer_hostport(0, hp, sizeof hp) &&
        strcmp(hp, "127.0.0.1:18080") == 0);
  CHECK(disc_peer_hostport(5610, hp, sizeof hp) &&
        strcmp(hp, "127.0.0.1:5610") == 0);

  kill(child, SIGKILL);
  waitpid(child, NULL, 0);
  for (int i = 0; i < 80 && v.state == DISC_FOUND; i++) {
    sleep_ms(100);
    disc_view(&v);
  }
  CHECK_EQ_INT(v.state, DISC_SEARCHING);
  CHECK(!disc_peer_hostport(0, hp, sizeof hp));
  disc_stop();
  disc_view(&v);
  CHECK_EQ_INT(v.state, DISC_OFF);
  unsetenv("HELLION_DISCOVERY_PORT");
  unsetenv("HELLION_DISCOVERY_TO");
}

int main(void) {
  snprintf(bus_dir, sizeof bus_dir, "/dev/shm/eeye-test-hellion-XXXXXX");
  if (!mkdtemp(bus_dir)) {
    snprintf(bus_dir, sizeof bus_dir, "/tmp/eeye-test-hellion-XXXXXX");
    if (!mkdtemp(bus_dir))
      return 1;
  }
  setenv("EEYE_BUS_DIR", bus_dir, 1);

  RUN_TEST(test_config_full_example);
  RUN_TEST(test_config_defaults);
  RUN_TEST(test_config_errors);
  RUN_TEST(test_pipeline_forwards_frames);
  RUN_TEST(test_pipeline_follows_size_change);
  RUN_TEST(test_failing_sink_does_not_starve_others);
  RUN_TEST(test_pipeline_waits_for_missing_source);
  RUN_TEST(test_config_rtp);
  RUN_TEST(test_config_camera);
  RUN_TEST(test_config_discovery);
  RUN_TEST(test_discovery_beacons);
  RUN_TEST(test_discovery_choice);
  RUN_TEST(test_discovery_two_processes);
  RUN_TEST(test_example_configs_load);
  RUN_TEST(test_udp_relay);
  RUN_TEST(test_udp_relay_frame_ends);
  RUN_TEST(test_rtp_round_trip);
  RUN_TEST(test_recorder);
  RUN_TEST(test_status_round_trip);
  RUN_TEST(test_log_capture);

  remove_dir(bus_dir);
  TEST_MAIN_END();
}
