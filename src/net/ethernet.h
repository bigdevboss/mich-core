#ifndef ETHERNET_H
#define ETHERNET_H

#include "types.h"
#include "object.h"

#define ETHERNET_ADDRESS_SIZE 6
#define ETHERNET_HEADER_SIZE 14
#define ETHERNET_VLAN_HEADER_SIZE 18
#define ETHERNET_FRAME_MAX_UNTAGGED 1518
#define ETHERNET_FRAME_MAX 1522
#define ETHERNET_HANDLER_MAX 8

#define ETHERNET_FLAG_BROADCAST (1u << 0)
#define ETHERNET_FLAG_MULTICAST (1u << 1)
#define ETHERNET_FLAG_VLAN (1u << 2)

struct ethernet_frame_view {
    const u8 *frame;
    const u8 *payload;
    u32 frame_length;
    u32 payload_length;
    u16 ether_type;
    u16 vlan_tci;
    u32 flags;
};

struct ethernet_stats {
    u64 frames;
    u64 bytes;
    u64 unicast;
    u64 broadcast;
    u64 multicast;
    u64 vlan;
    u64 drops_length;
    u64 drops_source;
    u64 drops_filter;
    u64 drops_protocol;
};

struct ethernet_port;
typedef int (*ethernet_handler_fn)(struct ethernet_port *port,
                                   const struct ethernet_frame_view *frame,
                                   void *context);

struct ethernet_handler {
    u16 ether_type;
    ethernet_handler_fn function;
    void *context;
};

struct ethernet_port {
    u8 address[ETHERNET_ADDRESS_SIZE];
    u32 promiscuous;
    u32 accept_multicast;
    struct ethernet_handler handlers[ETHERNET_HANDLER_MAX];
    u32 handler_count;
    struct ethernet_stats stats;
};

int ethernet_port_init(struct ethernet_port *port, const u8 address[6]);
int ethernet_port_set_filter(struct ethernet_port *port, int promiscuous,
                             int accept_multicast);
int ethernet_register_handler(struct ethernet_port *port, u16 ether_type,
                              ethernet_handler_fn function, void *context);
int ethernet_parse(const void *data, u32 length,
                   struct ethernet_frame_view *frame);
int ethernet_receive(struct ethernet_port *port, const void *data, u32 length);
int ethernet_receive_vnic(struct ethernet_port *port,
                          struct kernel_object *vnic);
u32 ethernet_receive_vnic_batch(struct ethernet_port *port,
                                struct kernel_object *vnic, u32 budget);

#endif
