#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Plug and play: hellions on the same network find each other.
 *
 * Every hellion with discovery on (the default) announces itself once a
 * second on UDP port 5612, and listens there for the others:
 *
 *   HELLION/1 role=rov name=rov pair=default placement=b control=8091
 *             tether=0 id=1a2b3c4d
 *
 * (one line). It goes to the IPv4 broadcast address of every interface
 * that is up and not loopback, and to IPv6 ff02::1 (all nodes) on every
 * one of them -- the second needs no address set up at all, so two
 * machines on a bare cable find each other with nothing configured.
 *
 * A hellion configured with "peer": "auto", or an RTP sink to
 * "rtp:auto:<port>", uses what it heard: the newest hellion of the
 * opposite role with the same "pair" name, heard in the last 5s. Two or
 * more such candidates and it does not guess: no peer, and the status says
 * so. A different placement is refused with the reason. A chosen peer
 * silent for 5s is dropped; whoever answers next is adopted.
 *
 * It trusts the network: there is no authentication, here or anywhere in
 * hellion's API. Anyone on the network can announce themselves as the
 * topside. "pair" keeps vehicles that share a network apart; it is not a
 * secret.
 */

enum {
  disc_port = 5612,
  disc_name_len = 32,
  disc_addr_len = 80,     // "fe80::1234:5678:9abc:def0%enp5s0" and more
  disc_detail_len = 160,
  disc_beacon_max = 255,  // a longer datagram is not a beacon
  disc_max_candidates = 16,
};

static const uint64_t disc_timeout_ns = 5000000000ull;

typedef struct {
  char role[16];               // "rov" or "topside"
  char name[disc_name_len];
  char pair[disc_name_len];
  char placement;              // 'a', 'b', or '?' (unknown)
  int control;                 // its control port, 0 = none
  int tether;                  // the RTP port it receives on, 0 = none
  uint32_t id;                 // random per process: tells beacons apart
} Beacon;

// The beacon as text (NUL-terminated). False if it does not fit `cap`.
bool disc_beacon_format(const Beacon *b, char *out, size_t cap);

// Parses one datagram. False for anything that is not a well-formed
// version-1 beacon: wrong magic or version, over disc_beacon_max bytes,
// control bytes, a missing role/pair/id, a bad value. Unknown keys are
// ignored, so a later version can add some.
bool disc_beacon_parse(const char *buf, size_t len, Beacon *out);

// What has been heard from one other hellion.
typedef struct {
  Beacon b;
  char addr4[disc_addr_len]; // where its IPv4 beacons came from ("" none)
  uint64_t seen4_ns;
  char addr6[disc_addr_len]; // IPv6, with %iface for a link-local one
  uint64_t seen6_ns;
  uint64_t seen_ns;          // newest of the two
} Candidate;

typedef enum {
  DISC_OFF = 0,      // discovery is turned off in the config
  DISC_SEARCHING,    // nothing usable heard
  DISC_FOUND,        // exactly one: chosen
  DISC_AMBIGUOUS,    // more than one answers our pair: refused to guess
} DiscState;

typedef struct {
  DiscState state;
  Beacon peer;                  // DISC_FOUND: the chosen one
  char addr[disc_addr_len];     // its address, IPv4 preferred
  uint64_t generation;          // changes whenever the choice does
  char detail[disc_detail_len]; // one line for people: why, or what
} DiscView;

typedef struct {
  const char *role;       // ours
  const char *pair;
  char placement;
} DiscWant;

// Picks the peer from what was heard (pure, for tests): the opposite
// role, the same pair, heard within disc_timeout_ns of `now`, placement
// not contradicting ours. Fills state, peer, addr and detail.
void disc_choose(const Candidate *c, size_t n, const DiscWant *want,
                 uint64_t now_ns, DiscView *out);

// ---- The running service (one per process) ----------------------------------

typedef struct {
  Beacon self;            // id is filled in by disc_start
} DiscConfig;

// Starts announcing and listening. False (with a message) if no socket
// could be opened; hellion runs on without discovery then.
bool disc_start(const DiscConfig *cfg);
void disc_stop(void);

// The current choice. DISC_OFF when not running.
void disc_view(DiscView *out);

// The chosen peer as "host:port" ("[v6%iface]:port" for IPv6), on `port`
// (0: its announced control port). False when there is none (or it
// announced no control port).
bool disc_peer_hostport(int port, char *out, size_t cap);
