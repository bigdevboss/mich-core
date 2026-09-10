#ifndef IPV4_H
#define IPV4_H

#include "types.h"
#include "ethernet.h"

#define IPV4_HEADER_MIN 20
#define IPV4_PROTOCOL_HANDLER_MAX 8

struct ipv4_packet_view {
    const u8 *packet;
    const u8 *header;
    const u8 *payload;
    struct kernel_object *packet_pool;
    u64 buffer_id;
    u32 packet_offset;
    u32 packet_length;
    u32 header_length;
    u32 payload_length;
    u32 source;
    u32 destination;
    u16 identification;
    u16 fragment;
    u8 ttl;
    u8 protocol;
    u8 dscp_ecn;
};

struct ipv4_stats {
    u64 packets;
    u64 bytes;
    u64 unicast;
    u64 broadcast;
    u64 drops_length;
    u64 drops_version;
    u64 drops_checksum;
    u64 drops_ttl;
    u64 drops_source;
    u64 drops_destination;
    u64 drops_fragment;
    u64 drops_protocol;
    u64 drops_land;
    u64 drops_spoof;
};

struct ipv4_context;
typedef int (*ipv4_transmit_fn)(struct ipv4_context *ipv4,
                                u32 destination, u8 protocol,
                                const void *payload, u32 payload_length,
                                void *context);
typedef int (*ipv4_protocol_fn)(struct ipv4_context *ipv4,
                                const struct ipv4_packet_view *packet,
                                void *context);

struct ipv4_protocol_handler {
    u8 protocol;
    ipv4_protocol_fn function;
    void *context;
};

struct ipv4_context {
    struct ethernet_port *port;
    u32 local_address;
    u32 netmask;
    u32 subnet_broadcast;
    u32 accept_broadcast;
    ipv4_transmit_fn transmit;
    void *transmit_context;
    struct ipv4_protocol_handler handlers[IPV4_PROTOCOL_HANDLER_MAX];
    u32 handler_count;
    struct ipv4_stats stats;
};

int ipv4_init(struct ipv4_context *ipv4, struct ethernet_port *port,
              u32 local_address, u32 netmask, int accept_broadcast);
int ipv4_set_transmit(struct ipv4_context *ipv4,
                      ipv4_transmit_fn transmit, void *context);
int ipv4_send(struct ipv4_context *ipv4, u32 destination, u8 protocol,
              const void *payload, u32 payload_length);
int ipv4_register_protocol(struct ipv4_context *ipv4, u8 protocol,
                           ipv4_protocol_fn function, void *context);
int ipv4_ethernet_handler(struct ethernet_port *port,
                          const struct ethernet_frame_view *frame,
                          void *context);
int ipv4_parse(const void *data, u32 length,
               struct ipv4_packet_view *packet);
u16 ipv4_checksum(const void *data, u32 length);
int ipv4_receive(struct ipv4_context *ipv4, const void *data, u32 length);
int ipv4_receive_buffer(struct ipv4_context *ipv4,
                        struct kernel_object *packet_pool, u64 buffer_id,
                        u32 offset, u32 length);
int ipv4_build_header(void *buffer, u32 buffer_length, u32 payload_length,
                      u32 source, u32 destination, u8 protocol, u8 ttl,
                      u16 identification, int dont_fragment);

#endif
