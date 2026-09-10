#ifndef ARP_H
#define ARP_H

#include "types.h"
#include "ethernet.h"

#define ARP_CACHE_MAX 32
#define ARP_PACKET_SIZE 28
#define ARP_REPLY_WINDOW 100
#define ARP_REPLY_MAX 32

#define ARP_CACHE_EMPTY 0
#define ARP_CACHE_PENDING 1
#define ARP_CACHE_REACHABLE 2

struct arp_cache_entry {
    u32 protocol_address;
    u8 hardware_address[6];
    u32 updated;
    u32 expires;
    u32 last_request;
    u32 state;
};

struct arp_stats {
    u64 requests_received;
    u64 replies_received;
    u64 requests_sent;
    u64 replies_sent;
    u64 cache_updates;
    u64 cache_expired;
    u64 malformed;
    u64 conflicts;
    u64 rate_limited;
};

typedef void (*arp_neighbor_update_fn)(u32 protocol_address,
                                       const u8 hardware_address[6],
                                       void *context);

typedef int (*arp_transmit_fn)(const u8 destination[6], u16 ether_type,
                               const void *payload, u32 length,
                               void *context);

struct arp_context {
    struct ethernet_port *port;
    u32 local_address;
    u32 cache_ttl;
    u32 request_interval;
    u32 now;
    u32 reply_window_start;
    u32 replies_in_window;
    struct arp_cache_entry cache[ARP_CACHE_MAX];
    struct arp_stats stats;
    arp_transmit_fn transmit;
    void *transmit_context;
    arp_neighbor_update_fn update;
    void *update_context;
};

int arp_init(struct arp_context *arp, struct ethernet_port *port,
             u32 local_address, u32 cache_ttl, u32 request_interval,
             arp_transmit_fn transmit, void *transmit_context);
int arp_set_update_callback(struct arp_context *arp,
                            arp_neighbor_update_fn update, void *context);
int arp_set_local_address(struct arp_context *arp, u32 local_address);
int arp_ethernet_handler(struct ethernet_port *port,
                         const struct ethernet_frame_view *frame,
                         void *context);
int arp_lookup(struct arp_context *arp, u32 protocol_address,
               u8 hardware_address[6], u32 now);
int arp_resolve(struct arp_context *arp, u32 protocol_address, u32 now);
void arp_tick(struct arp_context *arp, u32 now);
u32 arp_cache_count(const struct arp_context *arp, u32 state);

#endif
