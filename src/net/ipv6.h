#ifndef IPV6_H
#define IPV6_H

#include "types.h"
#include "ethernet.h"

#define IPV6_HEADER_SIZE 40
#define IPV6_ADDRESS_SIZE 16
#define IPV6_EXTENSION_MAX 8
#define IPV6_PROTOCOL_HANDLER_MAX 8
#define IPV6_LOCAL_ADDRESS_MAX 4

struct ipv6_packet_view {
    const u8 *packet;
    const u8 *payload;
    u8 source[IPV6_ADDRESS_SIZE];
    u8 destination[IPV6_ADDRESS_SIZE];
    u32 packet_length;
    u32 payload_offset;
    u32 payload_length;
    u32 flow_label;
    u8 traffic_class;
    u8 next_header;
    u8 hop_limit;
    u8 extension_count;
};

struct ipv6_stats {
    u64 packets;
    u64 bytes;
    u64 unicast;
    u64 multicast;
    u64 drops_length;
    u64 drops_version;
    u64 drops_hop_limit;
    u64 drops_source;
    u64 drops_destination;
    u64 drops_extension;
    u64 drops_fragment;
    u64 drops_protocol;
    u64 drops_land;
    u64 drops_spoof;
};

struct ipv6_context;
typedef int (*ipv6_protocol_fn)(struct ipv6_context *ipv6,
                                const struct ipv6_packet_view *packet,
                                void *context);

struct ipv6_protocol_handler {
    u8 protocol;
    ipv6_protocol_fn function;
    void *context;
};

struct ipv6_context {
    struct ethernet_port *port;
    u8 local_address[IPV6_ADDRESS_SIZE];
    u8 additional_addresses[IPV6_LOCAL_ADDRESS_MAX - 1][IPV6_ADDRESS_SIZE];
    u32 address_count;
    u32 accept_multicast;
    struct ipv6_protocol_handler handlers[IPV6_PROTOCOL_HANDLER_MAX];
    u32 handler_count;
    struct ipv6_stats stats;
};

int ipv6_init(struct ipv6_context *ipv6, struct ethernet_port *port,
              const u8 local_address[IPV6_ADDRESS_SIZE],
              int accept_multicast);
int ipv6_register_protocol(struct ipv6_context *ipv6, u8 protocol,
                           ipv6_protocol_fn function, void *context);
int ipv6_add_local_address(struct ipv6_context *ipv6,
                           const u8 address[IPV6_ADDRESS_SIZE]);
int ipv6_has_local_address(const struct ipv6_context *ipv6,
                           const u8 address[IPV6_ADDRESS_SIZE]);
int ipv6_remove_local_address(struct ipv6_context *ipv6,
                              const u8 address[IPV6_ADDRESS_SIZE]);
int ipv6_parse(const void *data, u32 length,
               struct ipv6_packet_view *packet);
int ipv6_receive(struct ipv6_context *ipv6, const void *data, u32 length);
int ipv6_ethernet_handler(struct ethernet_port *port,
                          const struct ethernet_frame_view *frame,
                          void *context);
int ipv6_build_header(void *buffer, u32 buffer_length, u32 payload_length,
                      const u8 source[IPV6_ADDRESS_SIZE],
                      const u8 destination[IPV6_ADDRESS_SIZE],
                      u8 next_header, u8 hop_limit,
                      u8 traffic_class, u32 flow_label);
int ipv6_address_equal(const u8 left[IPV6_ADDRESS_SIZE],
                       const u8 right[IPV6_ADDRESS_SIZE]);
int ipv6_address_unspecified(const u8 address[IPV6_ADDRESS_SIZE]);
int ipv6_address_multicast(const u8 address[IPV6_ADDRESS_SIZE]);
int ipv6_address_link_local(const u8 address[IPV6_ADDRESS_SIZE]);
int ipv6_link_local_from_mac(const u8 mac[6],
                             u8 address[IPV6_ADDRESS_SIZE]);
int ipv6_solicited_node_address(const u8 unicast[IPV6_ADDRESS_SIZE],
                                u8 multicast[IPV6_ADDRESS_SIZE]);

#endif
