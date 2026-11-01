#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A UDP packet relay that watches the RTP going through it.
 *
 * Every RTP stream hellion handles passes through one of these:
 * - receiving: bound to the tether port (e.g. 5601), it forwards each
 *   packet to the local ffmpeg decoder and to relay targets such as QGC;
 * - sending: bound to a local port the ffmpeg encoder sends to, it
 *   forwards each packet over the tether.
 *
 * Packets are forwarded unchanged and at once. Owning the socket is what
 * lets hellion report what the link is doing -- bitrate, packets, and
 * packet loss from the RTP sequence numbers -- and keeps relay targets fed
 * while a decoder restarts.
 */

typedef struct UdpRelay UdpRelay;

typedef struct {
  uint64_t packets;      // received
  uint64_t bytes;
  uint64_t lost;         // gaps in RTP sequence numbers
  uint64_t late;         // arrived after a later one (counted, not reordered)
  uint64_t send_errors;  // forwards that failed
  double kbps;           // over the last second or so
  double loss_pct;       // lost / (received + lost), over the same window
  double idle_s;         // since the last packet; -1 if none ever
} RelayStats;

/*
 * Binds `bind_spec`:
 *   "5601"               every interface, IPv6 and IPv4
 *   "127.0.0.1:0"        loopback, a port the kernel picks
 *   "<addr>:<port>"      that address ("[addr]:port" for IPv6)
 * NULL with *err filled on failure.
 */
UdpRelay *udp_relay_open(const char *bind_spec, char *err, size_t err_cap);

// The bound port (useful after binding port 0).
int udp_relay_port(const UdpRelay *r);

// Adds a destination, "host:port" or "[v6addr%iface]:port". Before start.
bool udp_relay_add_dest(UdpRelay *r, const char *dest, char *err,
                        size_t err_cap);

// Adds the local H.264 decoder as a destination. It gets the same packets,
// plus one more after each frame's last (RTP marker bit): a 2-byte access
// unit delimiter (AUD). ffmpeg's H.264 parser only knows a frame has
// ended when something after it arrives; without the AUD that is the next
// frame, a whole frame period later (33ms at 30fps), on every frame.
// Sequence numbers to this destination are shifted to make room, so the
// stream stays gap-free; other destinations (QGC, the recorder) get the
// stream unchanged. Before start.
bool udp_relay_add_decoder(UdpRelay *r, const char *dest, char *err,
                           size_t err_cap);

// Adds a destination while packets are flowing (a recording starting).
// Returns an id for udp_relay_detach, or -1 with *err filled.
int udp_relay_attach(UdpRelay *r, const char *dest, char *err,
                     size_t err_cap);

// Stops forwarding to a destination added by udp_relay_attach.
void udp_relay_detach(UdpRelay *r, int id);

// Starts the forwarding thread.
bool udp_relay_start(UdpRelay *r);

void udp_relay_stats(UdpRelay *r, RelayStats *out);

// Stops the thread and closes the socket. Safe on NULL.
void udp_relay_close(UdpRelay *r);

// Picks a free UDP port pair (even port, port+1 free too: RTP and RTCP) on
// 127.0.0.1, for an ffmpeg that has to bind it itself. 0 on failure.
int udp_free_port_pair(void);
