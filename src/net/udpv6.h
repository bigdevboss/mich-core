#ifndef UDPV6_H
#define UDPV6_H

#include "types.h"
#include "ipv6.h"

#define UDPV6_HEADER_SIZE 8
#define UDPV6_BINDING_MAX 16
#define UDPV6_QUEUE_MAX 4
#define UDPV6_PAYLOAD_MAX 1452
#define UDPV6_EPHEMERAL_FIRST 49152
#define UDPV6_EPHEMERAL_LAST 65535

typedef int (*udpv6_transmit_fn)(
    const u8 source[IPV6_ADDRESS_SIZE],
    const u8 destination[IPV6_ADDRESS_SIZE],
    const void *datagram, u32 length, void *context);

struct udpv6_datagram {
    u8 source[IPV6_ADDRESS_SIZE];
    u8 destination[IPV6_ADDRESS_SIZE];
    u16 source_port;
    u16 destination_port;
    u16 length;
    u8 payload[UDPV6_PAYLOAD_MAX];
};

struct udpv6_binding {
    u32 generation;
    u8 local_address[IPV6_ADDRESS_SIZE];
    u16 port;
    struct kernel_object *event;
    struct udpv6_datagram queue[UDPV6_QUEUE_MAX];
    u32 head;
    u32 count;
    u32 active;
};

struct udpv6_stats {
    u64 received;
    u64 transmitted;
    u64 bytes_received;
    u64 bytes_transmitted;
    u64 drops_length;
    u64 drops_checksum;
    u64 drops_port;
    u64 drops_queue;
    u64 drops_oversized;
};

struct udpv6_context {
    struct ipv6_context *ipv6;
    udpv6_transmit_fn transmit;
    void *transmit_context;
    u32 next_ephemeral;
    struct udpv6_binding bindings[UDPV6_BINDING_MAX];
    struct udpv6_stats stats;
};

u16 udpv6_checksum(const u8 source[IPV6_ADDRESS_SIZE],
                   const u8 destination[IPV6_ADDRESS_SIZE],
                   const void *datagram, u32 length);
int udpv6_init(struct udpv6_context *udp, struct ipv6_context *ipv6,
               udpv6_transmit_fn transmit, void *transmit_context);
u64 udpv6_bind(struct udpv6_context *udp,
               const u8 local_address[IPV6_ADDRESS_SIZE], u16 port);
int udpv6_unbind(struct udpv6_context *udp, u64 binding_id);
int udpv6_send(struct udpv6_context *udp, u64 binding_id,
               const u8 destination[IPV6_ADDRESS_SIZE], u16 destination_port,
               const void *payload, u32 payload_length);
int udpv6_receive(struct udpv6_context *udp, u64 binding_id,
                  struct udpv6_datagram *datagram);
struct kernel_object *udpv6_binding_event(struct udpv6_context *udp,
                                          u64 binding_id);
int udpv6_ipv6_handler(struct ipv6_context *ipv6,
                       const struct ipv6_packet_view *packet,
                       void *context);

#endif
