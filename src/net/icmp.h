#ifndef ICMP_H
#define ICMP_H

#include "types.h"
#include "ipv4.h"

#define ICMP_HEADER_SIZE 8
#define ICMP_ECHO_REPLY 0
#define ICMP_DESTINATION_UNREACHABLE 3
#define ICMP_FRAG_NEEDED 4
#define ICMP_ECHO_REQUEST 8
#define ICMP_TIME_EXCEEDED 11
#define ICMP_MESSAGE_MAX 1480

typedef void (*icmp_pmtu_fn)(u32 destination, u32 mtu, void *context);

struct icmp_stats {
    u64 echo_requests;
    u64 echo_replies;
    u64 replies_sent;
    u64 destination_unreachable;
    u64 fragmentation_needed;
    u64 time_exceeded;
    u64 malformed;
    u64 checksum_errors;
    u64 rate_limited;
    u64 broadcast_suppressed;
    u64 transmit_errors;
    u64 port_unreachable_sent;
};

struct icmp_context {
    struct ipv4_context *ipv4;
    icmp_pmtu_fn pmtu;
    void *pmtu_context;
    u32 now;
    u32 window_start;
    u32 window_ticks;
    u32 replies_in_window;
    u32 max_replies_per_window;
    struct icmp_stats stats;
};

int icmp_init(struct icmp_context *icmp, struct ipv4_context *ipv4,
              u32 window_ticks, u32 max_replies_per_window);
void icmp_tick(struct icmp_context *icmp, u32 now);
int icmp_ipv4_handler(struct ipv4_context *ipv4,
                      const struct ipv4_packet_view *packet,
                      void *context);
int icmp_send_port_unreachable(struct icmp_context *icmp,
                               const struct ipv4_packet_view *packet);
int icmp_set_pmtu_callback(struct icmp_context *icmp, icmp_pmtu_fn function,
                           void *context);
u16 icmp_checksum(const void *data, u32 length);

#endif
