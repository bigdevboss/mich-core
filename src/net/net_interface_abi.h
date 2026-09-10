#ifndef NET_INTERFACE_ABI_H
#define NET_INTERFACE_ABI_H

#include "types.h"

#define NET_INTERFACE_ABI_NAME_MAX 16
#define NET_INTERFACE_ABI_IPV6_DISABLED 0
#define NET_INTERFACE_ABI_IPV6_TENTATIVE 1
#define NET_INTERFACE_ABI_IPV6_LINK_LOCAL 2
#define NET_INTERFACE_ABI_IPV6_SLAAC 3

struct net_interface_create_request {
    u32 pool_handle;
    u32 rx_ring_handle;
    u32 tx_ring_handle;
    u32 mtu;
    u8 mac[6];
    u8 reserved0[2];
    char name[NET_INTERFACE_ABI_NAME_MAX];
    u32 interface_handle;
    u32 interface_id;
    u32 generation;
    u32 reserved1;
};

struct net_interface_info {
    u32 interface_id;
    u32 generation;
    u32 state;
    u32 mtu;
    u32 ipv4_address;
    u32 ipv4_netmask;
    u8 mac[6];
    u8 reserved[2];
    char name[NET_INTERFACE_ABI_NAME_MAX];
};

struct net_interface_buffer_request {
    u64 buffer_id;
    u32 offset;
    u32 length;
};

struct net_interface_ipv4_request {
    u32 address;
    u32 netmask;
    u32 gateway;
    u32 reserved;
};

struct net_interface_echo_request {
    u32 destination;
    u16 identifier;
    u16 sequence;
};

struct net_interface_ipv6_info {
    u32 state;
    u32 duplicate;
    u32 deprecated;
    u32 reserved;
    u64 echo_replies;
    u8 link_local[16];
    u8 global[16];
    u8 router[16];
};

#endif
