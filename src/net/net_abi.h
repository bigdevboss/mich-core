#ifndef NET_ABI_H
#define NET_ABI_H

#include "types.h"

#define NET_PACKET_DATA_MAX 4096
#define NET_PACKET_HEADROOM 128
#define NET_PACKET_DESCRIPTOR_SIZE 32

#define NET_BUFFER_FREE 0
#define NET_BUFFER_RX 1
#define NET_BUFFER_STACK 2
#define NET_BUFFER_TX 3
#define NET_BUFFER_TX_QUEUED 4
#define NET_BUFFER_DRIVER_RX 5
#define NET_BUFFER_DRIVER_TX 6

struct net_packet_descriptor {
    u64 buffer_id;
    u16 offset;
    u16 length;
    u16 flags;
    u16 reserved0;
    u32 checksum;
    u32 flow_hash;
    u64 timestamp;
};

struct vnic_create_request {
    u32 buffer_count;
    u32 ring_capacity;
    u32 vnic_handle;
    u32 pool_handle;
    u32 rx_ring_handle;
    u32 tx_ring_handle;
};

struct vnic_frame_request {
    u64 buffer_id;
    u32 user_address_low;
    u32 user_address_high;
    u16 offset;
    u16 length;
    u32 flags;
};

struct vnic_benchmark_request {
    u32 packets;
    u32 batch_size;
    u32 payload_size;
    u32 completed;
    u64 cycles;
};

typedef char net_packet_descriptor_size_check[
    sizeof(struct net_packet_descriptor) == NET_PACKET_DESCRIPTOR_SIZE ? 1 : -1];

#endif
