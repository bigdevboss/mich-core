#ifndef ICMPV6_H
#define ICMPV6_H

#include "types.h"
#include "ipv6.h"

#define ICMPV6_DESTINATION_UNREACHABLE 1
#define ICMPV6_PACKET_TOO_BIG 2
#define ICMPV6_TIME_EXCEEDED 3
#define ICMPV6_PARAMETER_PROBLEM 4
#define ICMPV6_ECHO_REQUEST 128
#define ICMPV6_ECHO_REPLY 129
#define ICMPV6_ROUTER_SOLICITATION 133
#define ICMPV6_ROUTER_ADVERTISEMENT 134
#define ICMPV6_NEIGHBOR_SOLICITATION 135
#define ICMPV6_NEIGHBOR_ADVERTISEMENT 136
#define ICMPV6_MESSAGE_MAX 1452
#define ICMPV6_NEIGHBOR_MAX 32
#define ICMPV6_REPLY_WINDOW 100
#define ICMPV6_REPLY_MAX 64

#define ICMPV6_NDP_OPTION_SOURCE_LL 1
#define ICMPV6_NDP_OPTION_TARGET_LL 2
#define ICMPV6_NDP_OPTION_PREFIX 3
#define ICMPV6_NDP_OPTION_MTU 5

#define ICMPV6_NEIGHBOR_INCOMPLETE 1
#define ICMPV6_NEIGHBOR_REACHABLE 2
#define ICMPV6_NEIGHBOR_STALE 3
#define ICMPV6_TICKS_PER_SECOND 100

typedef int (*icmpv6_transmit_fn)(const u8 source[IPV6_ADDRESS_SIZE],
                                  const u8 destination[IPV6_ADDRESS_SIZE],
                                  const void *message, u32 length,
                                  void *context);
typedef void (*icmpv6_pmtu_fn)(const u8 destination[IPV6_ADDRESS_SIZE],
                               u32 mtu, void *context);

struct icmpv6_neighbor {
    u8 address[IPV6_ADDRESS_SIZE];
    u8 mac[6];
    u32 state;
    u32 updated;
    u32 active;
};

struct icmpv6_stats {
    u64 echo_requests;
    u64 echo_replies;
    u64 replies_sent;
    u64 destination_unreachable;
    u64 packet_too_big;
    u64 time_exceeded;
    u64 parameter_problem;
    u64 router_solicitations;
    u64 router_advertisements;
    u64 neighbor_solicitations;
    u64 neighbor_advertisements;
    u64 neighbor_updates;
    u64 duplicate_addresses;
    u64 malformed;
    u64 checksum_errors;
    u64 hop_limit_errors;
    u64 option_errors;
    u64 transmit_errors;
    u64 rate_limited;
};

struct icmpv6_context {
    struct ipv6_context *ipv6;
    icmpv6_transmit_fn transmit;
    void *transmit_context;
    icmpv6_pmtu_fn pmtu;
    void *pmtu_context;
    struct icmpv6_neighbor neighbors[ICMPV6_NEIGHBOR_MAX];
    u8 local_address[IPV6_ADDRESS_SIZE];
    u8 local_mac[6];
    u8 router[IPV6_ADDRESS_SIZE];
    u8 prefix[IPV6_ADDRESS_SIZE];
    u32 router_lifetime;
    u32 prefix_valid_lifetime;
    u32 prefix_preferred_lifetime;
    u32 router_deadline;
    u32 prefix_valid_deadline;
    u32 prefix_preferred_deadline;
    u32 now;
    u32 window_start;
    u32 replies_in_window;
    u32 tentative;
    u32 duplicate;
    u32 prefix_autonomous;
    u8 prefix_length;
    struct icmpv6_stats stats;
};

u16 icmpv6_checksum(const u8 source[IPV6_ADDRESS_SIZE],
                    const u8 destination[IPV6_ADDRESS_SIZE],
                    const void *message, u32 length);
int icmpv6_init(struct icmpv6_context *icmpv6, struct ipv6_context *ipv6,
                const u8 local_address[IPV6_ADDRESS_SIZE],
                const u8 local_mac[6], int tentative,
                icmpv6_transmit_fn transmit, void *transmit_context);
int icmpv6_ipv6_handler(struct ipv6_context *ipv6,
                        const struct ipv6_packet_view *packet,
                        void *context);
int icmpv6_build_router_solicitation(
    const u8 source[IPV6_ADDRESS_SIZE], const u8 mac[6],
    const u8 destination[IPV6_ADDRESS_SIZE], void *buffer,
    u32 buffer_length, u32 *message_length);
int icmpv6_build_neighbor_solicitation(
    const u8 source[IPV6_ADDRESS_SIZE], const u8 target[IPV6_ADDRESS_SIZE],
    const u8 mac[6], void *buffer, u32 buffer_length,
    u8 destination[IPV6_ADDRESS_SIZE], u32 *message_length);
int icmpv6_neighbor_lookup(struct icmpv6_context *icmpv6,
                           const u8 address[IPV6_ADDRESS_SIZE],
                           u8 mac[6]);
int icmpv6_set_pmtu_callback(struct icmpv6_context *icmpv6,
                             icmpv6_pmtu_fn function, void *context);
void icmpv6_tick(struct icmpv6_context *icmpv6, u32 now);

#endif
