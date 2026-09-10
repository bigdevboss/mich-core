#ifndef UDP_H
#define UDP_H

#include "types.h"
#include "ipv4.h"

#define UDP_HEADER_SIZE 8
#define UDP_BINDING_MAX 32
#define UDP_QUEUE_MAX 8
#define UDP_PAYLOAD_MAX 1472
#define UDP_INLINE_PAYLOAD_MAX 512
#define UDP_EPHEMERAL_FIRST 49152
#define UDP_EPHEMERAL_LAST 65535

struct udp_datagram {
    u32 source_address;
    u32 destination_address;
    u16 source_port;
    u16 destination_port;
    u16 length;
    u16 reserved;
    struct kernel_object *packet_pool;
    u64 buffer_id;
    u32 payload_offset;
    u32 zero_copy;
    u8 payload[UDP_PAYLOAD_MAX];
};

struct udp_binding {
    u32 generation;
    u32 local_address;
    u16 port;
    u16 reserved;
    struct kernel_object *event;
    struct udp_datagram queue[UDP_QUEUE_MAX];
    u32 head;
    u32 count;
    u32 active;
};

struct udp_stats {
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

typedef int (*udp_unreachable_fn)(const struct ipv4_packet_view *packet,
                                  void *context);

struct udp_context {
    struct ipv4_context *ipv4;
    udp_unreachable_fn unreachable;
    void *unreachable_context;
    u32 next_ephemeral;
    struct udp_binding bindings[UDP_BINDING_MAX];
    struct udp_stats stats;
};

int udp_init(struct udp_context *udp, struct ipv4_context *ipv4);
int udp_set_unreachable_callback(struct udp_context *udp,
                                 udp_unreachable_fn function, void *context);
u64 udp_bind(struct udp_context *udp, u32 local_address, u16 port);
int udp_unbind(struct udp_context *udp, u64 binding_id);
int udp_send(struct udp_context *udp, u64 binding_id,
             u32 destination_address, u16 destination_port,
             const void *payload, u32 payload_length);
int udp_receive(struct udp_context *udp, u64 binding_id,
                struct udp_datagram *datagram);
struct kernel_object *udp_binding_event(struct udp_context *udp,
                                        u64 binding_id);
int udp_binding_local(struct udp_context *udp, u64 binding_id,
                      u32 *address, u16 *port);
int udp_ipv4_handler(struct ipv4_context *ipv4,
                     const struct ipv4_packet_view *packet,
                     void *context);
u16 udp_checksum(u32 source, u32 destination,
                 const void *datagram, u32 length);
u32 udp_binding_count(const struct udp_context *udp);

#endif
