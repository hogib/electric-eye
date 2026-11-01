// getaddrinfo, its AI_* flags and struct addrinfo are POSIX (2001).
#define _POSIX_C_SOURCE 200809L

#include "udp_relay.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { max_dests = 8, max_packet = 65536 };
static const uint64_t window_ns = 1000000000ull;

typedef struct {
  struct sockaddr_storage addr;
  socklen_t len;
  int fd; // a socket of the destination's family
} Dest;

struct UdpRelay {
  int fd;
  int port;
  Dest dests[max_dests];
  int dest_count;

  pthread_t thread;
  bool started;
  bool stop; // atomic (see compat.h)

  pthread_mutex_t lock; // everything below
  RelayStats totals;
  bool have_seq;
  uint16_t last_seq;
  uint32_t ssrc;
  uint64_t last_packet_ns;
  // Window for kbps / loss_pct.
  uint64_t win_start_ns;
  uint64_t win_bytes, win_packets, win_lost;
  double kbps, loss_pct;

  uint8_t buf[max_packet]; // one datagram, as received
};

static uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// "host:port", "[v6]:port" or just "port" -> host (may be empty) and port.
static bool split_host_port(const char *spec, char *host, size_t host_cap,
                            char *port, size_t port_cap) {
  host[0] = '\0';
  if (spec[0] == '[') {
    const char *close = strchr(spec, ']');
    if (!close || close[1] != ':')
      return false;
    snprintf(host, host_cap, "%.*s", (int)(close - spec - 1), spec + 1);
    snprintf(port, port_cap, "%s", close + 2);
  } else {
    const char *colon = strrchr(spec, ':');
    if (colon) {
      snprintf(host, host_cap, "%.*s", (int)(colon - spec), spec);
      snprintf(port, port_cap, "%s", colon + 1);
    } else {
      snprintf(port, port_cap, "%s", spec);
    }
  }
  char *end;
  long p = strtol(port, &end, 10);
  return port[0] && !*end && p >= 0 && p <= 65535;
}

UdpRelay *udp_relay_open(const char *bind_spec, char *err, size_t err_cap) {
  char host[128], port[16];
  if (!split_host_port(bind_spec, host, sizeof host, port, sizeof port)) {
    snprintf(err, err_cap, "bad address \"%s\"", bind_spec);
    return NULL;
  }

  int fd = -1;
  if (!host[0]) {
    // Every interface, both families: a v6 socket that also takes v4.
    fd = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
      int off = 0;
      setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
      struct sockaddr_in6 a6;
      memset(&a6, 0, sizeof a6);
      a6.sin6_family = AF_INET6;
      a6.sin6_addr = in6addr_any;
      a6.sin6_port = htons((uint16_t)atoi(port));
      if (bind(fd, (struct sockaddr *)&a6, sizeof a6) != 0) {
        close(fd);
        fd = -1;
      }
    }
    if (fd < 0) { // no IPv6 here: IPv4 only
      fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
      struct sockaddr_in a4;
      memset(&a4, 0, sizeof a4);
      a4.sin_family = AF_INET;
      a4.sin_addr.s_addr = htonl(INADDR_ANY);
      a4.sin_port = htons((uint16_t)atoi(port));
      if (fd < 0 || bind(fd, (struct sockaddr *)&a4, sizeof a4) != 0) {
        snprintf(err, err_cap, "cannot bind UDP port %s: %s", port,
                 strerror(errno));
        if (fd >= 0)
          close(fd);
        return NULL;
      }
    }
  } else {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
      snprintf(err, err_cap, "bad address \"%s\"", host);
      return NULL;
    }
    fd = socket(res->ai_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || bind(fd, res->ai_addr, res->ai_addrlen) != 0) {
      snprintf(err, err_cap, "cannot bind UDP %s: %s", bind_spec,
               strerror(errno));
      if (fd >= 0)
        close(fd);
      freeaddrinfo(res);
      return NULL;
    }
    freeaddrinfo(res);
  }

  // Room for a keyframe's burst of packets while the thread is scheduled
  // out. Best-effort: capped by net.core.rmem_max.
  int rcv = 4 << 20;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);

  UdpRelay *r = (UdpRelay *)calloc(1, sizeof *r);
  if (!r) {
    close(fd);
    return NULL;
  }
  r->fd = fd;
  struct sockaddr_storage ss;
  socklen_t sl = sizeof ss;
  getsockname(fd, (struct sockaddr *)&ss, &sl);
  r->port = ss.ss_family == AF_INET6
                ? ntohs(((struct sockaddr_in6 *)&ss)->sin6_port)
                : ntohs(((struct sockaddr_in *)&ss)->sin_port);
  pthread_mutex_init(&r->lock, NULL);
  return r;
}

int udp_relay_port(const UdpRelay *r) { return r ? r->port : 0; }

bool udp_relay_add_dest(UdpRelay *r, const char *dest, char *err,
                        size_t err_cap) {
  if (r->dest_count >= max_dests) {
    snprintf(err, err_cap, "more than %d destinations", max_dests);
    return false;
  }
  char host[128], port[16];
  if (!split_host_port(dest, host, sizeof host, port, sizeof port) ||
      !host[0]) {
    snprintf(err, err_cap, "bad destination \"%s\" (want host:port)", dest);
    return false;
  }
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_DGRAM;
  int rc = getaddrinfo(host, port, &hints, &res);
  if (rc != 0 || !res) {
    snprintf(err, err_cap, "cannot resolve \"%s\": %s", host,
             gai_strerror(rc));
    return false;
  }
  Dest *d = &r->dests[r->dest_count];
  memcpy(&d->addr, res->ai_addr, res->ai_addrlen);
  d->len = res->ai_addrlen;
  // Its own socket, so the family always matches the destination and
  // sends are never refused for that.
  d->fd = socket(res->ai_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  freeaddrinfo(res);
  if (d->fd < 0) {
    snprintf(err, err_cap, "socket: %s", strerror(errno));
    return false;
  }
  int snd = 4 << 20;
  setsockopt(d->fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof snd);
  r->dest_count++;
  return true;
}

// RTP bookkeeping for one packet; called with the lock held.
static void account(UdpRelay *r, const uint8_t *p, size_t n, uint64_t now) {
  r->totals.packets++;
  r->totals.bytes += n;
  r->win_packets++;
  r->win_bytes += n;
  r->last_packet_ns = now;

  // RTP: version 2 in the top two bits, 12-byte fixed header.
  if (n < 12 || (p[0] >> 6) != 2)
    return;
  uint16_t seq = (uint16_t)((p[2] << 8) | p[3]);
  uint32_t ssrc = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) |
                  ((uint32_t)p[10] << 8) | p[11];
  if (!r->have_seq || ssrc != r->ssrc) {
    // First packet, or the sender restarted (new SSRC): no gap to count.
    r->have_seq = true;
    r->ssrc = ssrc;
    r->last_seq = seq;
    return;
  }
  uint16_t delta = (uint16_t)(seq - r->last_seq);
  if (delta == 0)
    return; // duplicate
  if (delta < 0x8000) {
    r->totals.lost += (uint64_t)(delta - 1);
    r->win_lost += (uint64_t)(delta - 1);
    r->last_seq = seq;
  } else {
    // Behind the newest: a late packet. It was counted as lost when the
    // gap opened; take that back.
    r->totals.late++;
    if (r->totals.lost)
      r->totals.lost--;
    if (r->win_lost)
      r->win_lost--;
  }
}

static void roll_window(UdpRelay *r, uint64_t now) {
  if (r->win_start_ns == 0) {
    r->win_start_ns = now;
    return;
  }
  uint64_t span = now - r->win_start_ns;
  if (span < window_ns)
    return;
  r->kbps = (double)r->win_bytes * 8.0 / 1000.0 / ((double)span / 1e9);
  uint64_t expected = r->win_packets + r->win_lost;
  r->loss_pct = expected ? 100.0 * (double)r->win_lost / (double)expected : 0;
  r->win_start_ns = now;
  r->win_bytes = r->win_packets = r->win_lost = 0;
}

static void *relay_loop(void *arg) {
  UdpRelay *r = (UdpRelay *)arg;
  uint8_t *buf = r->buf;
  while (!__atomic_load_n(&r->stop, __ATOMIC_ACQUIRE)) {
    struct pollfd pfd = {.fd = r->fd, .events = POLLIN, .revents = 0};
    int pr = poll(&pfd, 1, 250);
    uint64_t now = mono_ns();
    if (pr <= 0) {
      pthread_mutex_lock(&r->lock);
      roll_window(r, now);
      pthread_mutex_unlock(&r->lock);
      continue;
    }
    // Drain everything waiting before polling again.
    for (;;) {
      ssize_t n = recv(r->fd, buf, max_packet, MSG_DONTWAIT);
      if (n < 0)
        break;
      uint64_t errors = 0;
      for (int i = 0; i < r->dest_count; i++) {
        Dest *d = &r->dests[i];
        if (sendto(d->fd, buf, (size_t)n, MSG_DONTWAIT,
                   (struct sockaddr *)&d->addr, d->len) < 0)
          errors++;
      }
      pthread_mutex_lock(&r->lock);
      account(r, buf, (size_t)n, now);
      r->totals.send_errors += errors;
      roll_window(r, now);
      pthread_mutex_unlock(&r->lock);
    }
  }
  return NULL;
}

bool udp_relay_start(UdpRelay *r) {
  r->started = pthread_create(&r->thread, NULL, relay_loop, r) == 0;
  return r->started;
}

void udp_relay_stats(UdpRelay *r, RelayStats *out) {
  memset(out, 0, sizeof *out);
  if (!r)
    return;
  uint64_t now = mono_ns();
  pthread_mutex_lock(&r->lock);
  *out = r->totals;
  out->kbps = r->kbps;
  out->loss_pct = r->loss_pct;
  out->idle_s =
      r->last_packet_ns ? (double)(now - r->last_packet_ns) / 1e9 : -1.0;
  // A link that went quiet should not keep reporting its last bitrate.
  if (out->idle_s < 0 || out->idle_s > 2.0) {
    out->kbps = 0;
    out->loss_pct = 0;
  }
  pthread_mutex_unlock(&r->lock);
}

void udp_relay_close(UdpRelay *r) {
  if (!r)
    return;
  __atomic_store_n(&r->stop, true, __ATOMIC_RELEASE);
  if (r->started)
    pthread_join(r->thread, NULL);
  close(r->fd);
  for (int i = 0; i < r->dest_count; i++)
    close(r->dests[i].fd);
  pthread_mutex_destroy(&r->lock);
  free(r);
}

int udp_free_port_pair(void) {
  for (int attempt = 0; attempt < 50; attempt++) {
    int a = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    socklen_t sl = sizeof sa;
    if (a < 0 || bind(a, (struct sockaddr *)&sa, sizeof sa) != 0 ||
        getsockname(a, (struct sockaddr *)&sa, &sl) != 0) {
      if (a >= 0)
        close(a);
      return 0;
    }
    int port = ntohs(sa.sin_port);
    close(a);
    if (port % 2 || port >= 65534)
      continue;
    // Check port+1 too (RTCP), then hand both back. Racy in principle
    // (something else could take them before ffmpeg does); a failure then
    // just shows as the decoder exiting and being restarted.
    int b = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sa.sin_port = htons((uint16_t)(port + 1));
    bool ok = b >= 0 && bind(b, (struct sockaddr *)&sa, sizeof sa) == 0;
    if (b >= 0)
      close(b);
    if (ok)
      return port;
  }
  return 0;
}
