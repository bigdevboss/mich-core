#ifndef VNIC_H
#define VNIC_H

#include "types.h"
#include "object.h"
#include "net_abi.h"

#define VNIC_MAX 8

struct vnic_stats {
    u64 rx_packets;
    u64 rx_drops;
    u64 tx_packets;
    u64 tx_drops;
};

void vnic_init(void);
struct kernel_object *vnic_create(u32 buffer_count, u32 ring_capacity);
struct kernel_object *vnic_pool(struct kernel_object *vnic);
struct kernel_object *vnic_rx_ring(struct kernel_object *vnic);
struct kernel_object *vnic_tx_ring(struct kernel_object *vnic);
int vnic_inject(struct kernel_object *vnic, const void *frame, u32 length);
int vnic_receive(struct kernel_object *vnic,
                 struct net_packet_descriptor *descriptor);
int vnic_release_rx(struct kernel_object *vnic, u64 buffer_id);
u64 vnic_acquire_tx(struct kernel_object *vnic);
int vnic_submit_tx(struct kernel_object *vnic, u64 buffer_id,
                   u32 offset, u32 length, u32 flags);
int vnic_drain_tx(struct kernel_object *vnic,
                  struct net_packet_descriptor *descriptor);
int vnic_revoke(struct kernel_object *vnic);
int vnic_get_stats(struct kernel_object *vnic, struct vnic_stats *stats);
u32 vnic_active_count(void);

#endif
