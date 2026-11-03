// getifaddrs() and the IFF_* interface flags are BSD/"default" extensions,
// not POSIX: _DEFAULT_SOURCE on glibc 2.19 and later, _BSD_SOURCE before.
// (No strchr and friends in this file: with these macros, newer glibc
// turns them into _Generic macros that clang warns about.)
#define _DEFAULT_SOURCE
#define _BSD_SOURCE

#include "thread_name.h"
#include "discovery.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// ---- Beacons ------------------------------------------------------------------

static const char magic[] = "HELLION/1";

static bool name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static bool valid_name(const char *s) {
  if (!s[0])
    return false;
  for (const char *c = s; *c; c++)
    if (!name_char(*c))
      return false;
  return true;
}

bool disc_beacon_format(const Beacon *b, char *out, size_t cap) {
  int n = snprintf(out, cap,
                   "%s role=%s name=%s pair=%s placement=%c control=%d "
                   "tether=%d id=%08x",
                   magic, b->role, b->name[0] ? b->name : "-", b->pair,
                   b->placement ? b->placement : '?', b->control, b->tether,
                   (unsigned)b->id);
  return n > 0 && (size_t)n < cap && n <= disc_beacon_max;
}

// Copies the value of `key=` from one token into out. True when the token
// is that key.
static bool token_value(const char *tok, size_t len, const char *key,
                        char *out, size_t cap) {
  size_t k = strlen(key);
  if (len <= k || memcmp(tok, key, k) != 0 || tok[k] != '=')
    return false;
  size_t v = len - k - 1;
  if (v >= cap)
    v = cap - 1; // too long: truncated, and then rejected by validation
  memcpy(out, tok + k + 1, v);
  out[v] = '\0';
  return true;
}

static bool parse_uint(const char *s, unsigned long max, unsigned long *out,
                       int base) {
  if (!s[0])
    return false;
  unsigned long v = 0;
  for (const char *c = s; *c; c++) {
    int d;
    if (*c >= '0' && *c <= '9')
      d = *c - '0';
    else if (base == 16 && *c >= 'a' && *c <= 'f')
      d = *c - 'a' + 10;
    else if (base == 16 && *c >= 'A' && *c <= 'F')
      d = *c - 'A' + 10;
    else
      return false;
    v = v * (unsigned long)base + (unsigned long)d;
    if (v > max)
      return false;
  }
  *out = v;
  return true;
}

bool disc_beacon_parse(const char *buf, size_t len, Beacon *out) {
  memset(out, 0, sizeof *out);
  out->placement = '?';
  // A trailing newline is allowed; any other control byte is not.
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
    len--;
  if (len == 0 || len > disc_beacon_max)
    return false;
  for (size_t i = 0; i < len; i++)
    if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] > 0x7e)
      return false;
  size_t m = sizeof magic - 1;
  if (len < m || memcmp(buf, magic, m) != 0 || (len > m && buf[m] != ' '))
    return false;

  bool have_role = false, have_pair = false, have_id = false;
  size_t i = m;
  while (i < len) {
    while (i < len && buf[i] == ' ')
      i++;
    size_t start = i;
    while (i < len && buf[i] != ' ')
      i++;
    const char *tok = buf + start;
    size_t tlen = i - start;
    if (tlen == 0)
      break;
    char v[64];
    unsigned long n;
    if (token_value(tok, tlen, "role", v, sizeof v)) {
      if (strcmp(v, "rov") != 0 && strcmp(v, "topside") != 0)
        return false;
      snprintf(out->role, sizeof out->role, "%.15s", v); // "topside" at most
      have_role = true;
    } else if (token_value(tok, tlen, "name", v, sizeof v)) {
      if (strcmp(v, "-") != 0) {
        if (strlen(v) >= disc_name_len || !valid_name(v))
          return false;
        snprintf(out->name, sizeof out->name, "%s", v);
      }
    } else if (token_value(tok, tlen, "pair", v, sizeof v)) {
      if (strlen(v) >= disc_name_len || !valid_name(v))
        return false;
      snprintf(out->pair, sizeof out->pair, "%s", v);
      have_pair = true;
    } else if (token_value(tok, tlen, "placement", v, sizeof v)) {
      if (strlen(v) != 1 || (v[0] != 'a' && v[0] != 'b' && v[0] != '?'))
        return false;
      out->placement = v[0];
    } else if (token_value(tok, tlen, "control", v, sizeof v)) {
      if (!parse_uint(v, 65535, &n, 10))
        return false;
      out->control = (int)n;
    } else if (token_value(tok, tlen, "tether", v, sizeof v)) {
      if (!parse_uint(v, 65535, &n, 10))
        return false;
      out->tether = (int)n;
    } else if (token_value(tok, tlen, "id", v, sizeof v)) {
      if (strlen(v) != 8 || !parse_uint(v, 0xffffffffUL, &n, 16))
        return false;
      out->id = (uint32_t)n;
      have_id = true;
    }
    // anything else: a key from a later version, ignored
  }
  return have_role && have_pair && have_id;
}

// ---- Choosing -------------------------------------------------------------------

static void add_detail(char *out, size_t cap, const char *fmt, const char *a,
                       const char *b) {
  size_t n = strlen(out);
  if (n + 1 >= cap)
    return;
  snprintf(out + n, cap - n, fmt, a, b);
}

static const char *best_addr(const Candidate *c, uint64_t now) {
  if (c->addr4[0] && now - c->seen4_ns <= disc_timeout_ns)
    return c->addr4;
  if (c->addr6[0] && now - c->seen6_ns <= disc_timeout_ns)
    return c->addr6;
  return c->addr4[0] ? c->addr4 : c->addr6;
}

void disc_choose(const Candidate *c, size_t n, const DiscWant *want,
                 uint64_t now_ns, DiscView *out) {
  uint64_t generation = out->generation;
  memset(out, 0, sizeof *out);
  out->generation = generation;
  const char *other = strcmp(want->role, "rov") == 0 ? "topside" : "rov";

  const Candidate *match[disc_max_candidates];
  size_t matches = 0;
  char refused[disc_detail_len] = "";
  for (size_t i = 0; i < n; i++) {
    const Candidate *k = &c[i];
    if (k->seen_ns == 0 || now_ns - k->seen_ns > disc_timeout_ns)
      continue;
    if (strcmp(k->b.role, other) != 0)
      continue;
    const char *addr = best_addr(k, now_ns);
    if (strcmp(k->b.pair, want->pair) != 0) {
      char why[64];
      snprintf(why, sizeof why, "pair \"%.20s\"", k->b.pair);
      add_detail(refused, sizeof refused, "%s%s", refused[0] ? "; " : "", "");
      add_detail(refused, sizeof refused, "%s has %s", addr, why);
      continue;
    }
    if (want->placement != '?' && k->b.placement != '?' &&
        k->b.placement != want->placement) {
      char why[64];
      snprintf(why, sizeof why, "placement %c, this one %c", k->b.placement,
               want->placement);
      add_detail(refused, sizeof refused, "%s%s", refused[0] ? "; " : "", "");
      add_detail(refused, sizeof refused, "%s is %s", addr, why);
      continue;
    }
    if (matches < disc_max_candidates)
      match[matches++] = k;
  }

  if (matches == 1) {
    out->state = DISC_FOUND;
    out->peer = match[0]->b;
    snprintf(out->addr, sizeof out->addr, "%s", best_addr(match[0], now_ns));
    snprintf(out->detail, sizeof out->detail, "%s \"%s\" at %s", other,
             match[0]->b.name[0] ? match[0]->b.name : "?", out->addr);
  } else if (matches > 1) {
    out->state = DISC_AMBIGUOUS;
    snprintf(out->detail, sizeof out->detail, "%zu %ss answer pair \"%s\": ",
             matches, other, want->pair);
    for (size_t i = 0; i < matches && i < 3; i++)
      add_detail(out->detail, sizeof out->detail, "%s%s", i ? ", " : "",
                 best_addr(match[i], now_ns));
    add_detail(out->detail, sizeof out->detail, "%s%s",
               " -- give each vehicle its own --pair", "");
  } else {
    out->state = DISC_SEARCHING;
    if (refused[0])
      snprintf(out->detail, sizeof out->detail, "no %s for pair \"%s\" (%s)",
               other, want->pair, refused);
    else
      snprintf(out->detail, sizeof out->detail,
               "no %s heard on the network yet (pair \"%s\")", other,
               want->pair);
  }
}

// ---- The service ------------------------------------------------------------------

static uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

enum { max_ifaces = 32 };

typedef struct {
  DiscConfig cfg;
  int port;
  int fd4, fd6;
  pthread_t thread;
  bool stop; // atomic (see compat.h)

  // Interfaces, re-read every few seconds: a tether is often plugged in
  // after hellion has started.
  struct sockaddr_in bcast[max_ifaces];
  int bcast_count;
  unsigned v6_ifaces[max_ifaces];
  int v6_count;
  unsigned joined[max_ifaces]; // ff02::1 memberships already taken
  int joined_count;

  // Test-only: beacons go to these unicast addresses instead of broadcast
  // and multicast (HELLION_DISCOVERY_TO), since broadcast does not work
  // on loopback. Never set in normal use.
  struct sockaddr_storage test_to[4];
  socklen_t test_to_len[4];
  int test_to_count;

  pthread_mutex_t lock; // the fields below
  Candidate cand[disc_max_candidates];
  DiscView view;
} Disc;

static Disc *g_disc; // the running service, if any
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void read_interfaces(Disc *d) {
  struct ifaddrs *list = NULL;
  d->bcast_count = 0;
  d->v6_count = 0;
  if (getifaddrs(&list) != 0)
    return;
  for (struct ifaddrs *i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || !(i->ifa_flags & IFF_UP) ||
        (i->ifa_flags & IFF_LOOPBACK))
      continue;
    if (i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_BROADCAST) &&
        i->ifa_broadaddr && d->bcast_count < max_ifaces) {
      struct sockaddr_in a;
      memcpy(&a, i->ifa_broadaddr, sizeof a);
      a.sin_port = htons((uint16_t)d->port);
      d->bcast[d->bcast_count++] = a;
    } else if (i->ifa_addr->sa_family == AF_INET6 &&
               (i->ifa_flags & IFF_MULTICAST)) {
      unsigned idx = if_nametoindex(i->ifa_name);
      bool seen = idx == 0;
      for (int k = 0; k < d->v6_count && !seen; k++)
        seen = d->v6_ifaces[k] == idx;
      if (!seen && d->v6_count < max_ifaces)
        d->v6_ifaces[d->v6_count++] = idx;
    }
  }
  freeifaddrs(list);

  // Listen for ff02::1 on each, once.
  if (d->fd6 < 0)
    return;
  for (int k = 0; k < d->v6_count; k++) {
    bool done = false;
    for (int j = 0; j < d->joined_count && !done; j++)
      done = d->joined[j] == d->v6_ifaces[k];
    if (done || d->joined_count >= max_ifaces)
      continue;
    struct ipv6_mreq mr;
    memset(&mr, 0, sizeof mr);
    inet_pton(AF_INET6, "ff02::1", &mr.ipv6mr_multiaddr);
    mr.ipv6mr_interface = d->v6_ifaces[k];
    if (setsockopt(d->fd6, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mr, sizeof mr) ==
            0 ||
        errno == EADDRINUSE)
      d->joined[d->joined_count++] = d->v6_ifaces[k];
  }
}

static int open_socket(int family, int port) {
  int fd = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  int one = 1;
  // Several hellions on one machine (a bench test) all listen here.
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (family == AF_INET) {
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
      close(fd);
      return -1;
    }
  } else {
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_any;
    a.sin6_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
      close(fd);
      return -1;
    }
  }
  int fl = fcntl(fd, F_GETFL);
  if (fl >= 0)
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  return fd;
}

static void announce(Disc *d) {
  char text[disc_beacon_max + 1];
  if (!disc_beacon_format(&d->cfg.self, text, sizeof text))
    return;
  size_t n = strlen(text);
  if (d->test_to_count) {
    for (int i = 0; i < d->test_to_count; i++) {
      int fd = d->test_to[i].ss_family == AF_INET6 ? d->fd6 : d->fd4;
      if (fd >= 0)
        sendto(fd, text, n, MSG_DONTWAIT, (struct sockaddr *)&d->test_to[i],
               d->test_to_len[i]);
    }
    return;
  }
  if (d->fd4 >= 0)
    for (int i = 0; i < d->bcast_count; i++)
      sendto(d->fd4, text, n, MSG_DONTWAIT, (struct sockaddr *)&d->bcast[i],
             sizeof d->bcast[i]);
  if (d->fd6 >= 0)
    for (int i = 0; i < d->v6_count; i++) {
      struct sockaddr_in6 a;
      memset(&a, 0, sizeof a);
      a.sin6_family = AF_INET6;
      inet_pton(AF_INET6, "ff02::1", &a.sin6_addr);
      a.sin6_port = htons((uint16_t)d->port);
      a.sin6_scope_id = d->v6_ifaces[i];
      unsigned idx = d->v6_ifaces[i];
      setsockopt(d->fd6, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx, sizeof idx);
      sendto(d->fd6, text, n, MSG_DONTWAIT, (struct sockaddr *)&a, sizeof a);
    }
}

// Records one beacon from `from`. Called with d->lock held.
static void heard(Disc *d, const Beacon *b, const struct sockaddr_storage *from,
                  uint64_t now) {
  if (b->id == d->cfg.self.id)
    return; // our own, looped back
  char addr[disc_addr_len] = "";
  bool v4 = from->ss_family == AF_INET;
  if (v4) {
    const struct sockaddr_in *a = (const struct sockaddr_in *)from;
    inet_ntop(AF_INET, &a->sin_addr, addr, sizeof addr);
  } else {
    const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)from;
    char host[INET6_ADDRSTRLEN] = "";
    inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof host);
    char ifname[IF_NAMESIZE] = "";
    if (a->sin6_scope_id && if_indextoname(a->sin6_scope_id, ifname))
      snprintf(addr, sizeof addr, "%s%%%s", host, ifname);
    else
      snprintf(addr, sizeof addr, "%s", host);
  }
  Candidate *slot = NULL, *oldest = &d->cand[0];
  for (int i = 0; i < disc_max_candidates; i++) {
    Candidate *k = &d->cand[i];
    if (k->seen_ns && k->b.id == b->id) {
      slot = k;
      break;
    }
    if (k->seen_ns < oldest->seen_ns)
      oldest = k;
  }
  if (!slot) {
    slot = oldest;
    memset(slot, 0, sizeof *slot);
  }
  slot->b = *b;
  if (v4) {
    snprintf(slot->addr4, sizeof slot->addr4, "%s", addr);
    slot->seen4_ns = now;
  } else {
    snprintf(slot->addr6, sizeof slot->addr6, "%s", addr);
    slot->seen6_ns = now;
  }
  slot->seen_ns = now;
}

static void rechoose(Disc *d, uint64_t now) {
  DiscWant want = {d->cfg.self.role, d->cfg.self.pair, d->cfg.self.placement};
  DiscView old = d->view;
  DiscView v;
  v.generation = old.generation;
  disc_choose(d->cand, disc_max_candidates, &want, now, &v);
  if (v.state != old.state || strcmp(v.addr, old.addr) != 0 ||
      v.peer.id != old.peer.id) {
    v.generation = old.generation + 1;
    printf("hellion: discovery: %s\n", v.detail);
  }
  d->view = v;
}

static void receive(Disc *d, int fd) {
  // Bounded: a flood on the port (anything on the network can send to it)
  // must not keep this thread here, unable to announce or re-choose.
  for (int i = 0; i < 64; i++) {
    char buf[disc_beacon_max + 2];
    struct sockaddr_storage from;
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(fd, buf, sizeof buf, MSG_DONTWAIT,
                         (struct sockaddr *)&from, &fl);
    if (n < 0)
      return;
    Beacon b;
    if (!disc_beacon_parse(buf, (size_t)n, &b))
      continue;
    pthread_mutex_lock(&d->lock);
    heard(d, &b, &from, mono_ns());
    pthread_mutex_unlock(&d->lock);
  }
}

static void *disc_loop(void *arg) {
  thread_set_name("hl/discovery");
  Disc *d = (Disc *)arg;
  uint64_t next_announce = 0, next_ifaces = 0;
  while (!__atomic_load_n(&d->stop, __ATOMIC_ACQUIRE)) {
    uint64_t now = mono_ns();
    if (now >= next_ifaces) {
      read_interfaces(d);
      next_ifaces = now + 5000000000ull;
    }
    if (now >= next_announce) {
      announce(d);
      next_announce = now + 1000000000ull;
    }
    struct pollfd p[2];
    int np = 0;
    if (d->fd4 >= 0)
      p[np++] = (struct pollfd){.fd = d->fd4, .events = POLLIN};
    if (d->fd6 >= 0)
      p[np++] = (struct pollfd){.fd = d->fd6, .events = POLLIN};
    // Woken by beacons; otherwise every 250ms, to announce on time and
    // notice a peer that went quiet.
    if (poll(p, (nfds_t)np, 250) > 0)
      for (int i = 0; i < np; i++)
        if (p[i].revents & POLLIN)
          receive(d, p[i].fd);
    pthread_mutex_lock(&d->lock);
    rechoose(d, mono_ns());
    pthread_mutex_unlock(&d->lock);
  }
  return NULL;
}

static uint32_t random_id(void) {
  uint32_t id = 0;
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    if (read(fd, &id, sizeof id) != (ssize_t)sizeof id)
      id = 0;
    close(fd);
  }
  if (id == 0)
    id = (uint32_t)(mono_ns() ^ ((uint64_t)getpid() << 16));
  return id ? id : 1;
}

// HELLION_DISCOVERY_TO: "host:port[,host:port...]", IPv4 only. Tests.
static void read_test_targets(Disc *d) {
  const char *env = getenv("HELLION_DISCOVERY_TO");
  if (!env || !env[0])
    return;
  char buf[256];
  snprintf(buf, sizeof buf, "%s", env);
  char *save = NULL;
  for (char *t = strtok_r(buf, ",", &save); t && d->test_to_count < 4;
       t = strtok_r(NULL, ",", &save)) {
    char *colon = NULL;
    for (char *c = t; *c; c++)
      if (*c == ':')
        colon = c;
    if (!colon)
      continue;
    *colon = '\0';
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)atoi(colon + 1));
    if (inet_pton(AF_INET, t, &a.sin_addr) != 1)
      continue;
    memcpy(&d->test_to[d->test_to_count], &a, sizeof a);
    d->test_to_len[d->test_to_count++] = sizeof a;
  }
}

bool disc_start(const DiscConfig *cfg) {
  Disc *d = (Disc *)calloc(1, sizeof *d);
  if (!d)
    return false;
  d->cfg = *cfg;
  d->cfg.self.id = random_id();
  if (!d->cfg.self.placement)
    d->cfg.self.placement = '?';
  // HELLION_DISCOVERY_PORT: a different port, for tests running several
  // pairs on one machine. Normal use never sets it.
  const char *port = getenv("HELLION_DISCOVERY_PORT");
  d->port = port && atoi(port) > 0 ? atoi(port) : disc_port;
  read_test_targets(d);
  d->fd4 = open_socket(AF_INET, d->port);
  d->fd6 = open_socket(AF_INET6, d->port);
  if (d->fd4 < 0 && d->fd6 < 0) {
    printf("hellion: discovery off: cannot listen on UDP %d: %s\n", d->port,
           strerror(errno));
    free(d);
    return false;
  }
  pthread_mutex_init(&d->lock, NULL);
  d->view.state = DISC_SEARCHING;
  snprintf(d->view.detail, sizeof d->view.detail, "listening");
  __atomic_store_n(&d->stop, false, __ATOMIC_RELEASE);
  if (pthread_create(&d->thread, NULL, disc_loop, d) != 0) {
    if (d->fd4 >= 0)
      close(d->fd4);
    if (d->fd6 >= 0)
      close(d->fd6);
    pthread_mutex_destroy(&d->lock);
    free(d);
    return false;
  }
  printf("hellion: discovery on UDP %d as %s \"%s\", pair \"%s\" (id %08x)\n",
         d->port, d->cfg.self.role, d->cfg.self.name, d->cfg.self.pair,
         (unsigned)d->cfg.self.id);
  pthread_mutex_lock(&g_lock);
  g_disc = d;
  pthread_mutex_unlock(&g_lock);
  return true;
}

void disc_stop(void) {
  pthread_mutex_lock(&g_lock);
  Disc *d = g_disc;
  g_disc = NULL;
  pthread_mutex_unlock(&g_lock);
  if (!d)
    return;
  __atomic_store_n(&d->stop, true, __ATOMIC_RELEASE);
  pthread_join(d->thread, NULL);
  if (d->fd4 >= 0)
    close(d->fd4);
  if (d->fd6 >= 0)
    close(d->fd6);
  pthread_mutex_destroy(&d->lock);
  free(d);
}

void disc_view(DiscView *out) {
  memset(out, 0, sizeof *out);
  out->state = DISC_OFF;
  pthread_mutex_lock(&g_lock);
  Disc *d = g_disc;
  if (d) {
    pthread_mutex_lock(&d->lock);
    *out = d->view;
    pthread_mutex_unlock(&d->lock);
  }
  pthread_mutex_unlock(&g_lock);
}

bool disc_peer_hostport(int port, char *out, size_t cap) {
  DiscView v;
  disc_view(&v);
  if (v.state != DISC_FOUND)
    return false;
  if (port == 0)
    port = v.peer.control;
  if (port <= 0)
    return false;
  bool v6 = false;
  for (const char *c = v.addr; *c; c++)
    v6 |= *c == ':';
  int n = snprintf(out, cap, v6 ? "[%s]:%d" : "%s:%d", v.addr, port);
  return n > 0 && (size_t)n < cap;
}
