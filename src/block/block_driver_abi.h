#ifndef BLOCK_DRIVER_ABI_H
#define BLOCK_DRIVER_ABI_H

#include "types.h"

// Contract between the kernel block layer and a userspace block-driver capsule:
// a request ring (kernel -> capsule), a completion ring (capsule -> kernel), and
// a shared data pool. Mirrors net_interface_abi.h.

#define BLOCK_DRIVER_ABI_NAME_MAX 16

// Ring capacity for the kernel <-> capsule transport. Must be a power of two and
// at most RING_CAPACITY_MAX; 32 sits comfortably above the in-kernel in-flight
// bound (BLOCK_REQUEST_MAX in block.h, 16) so a valid submit never finds the ring
// full. Kept distinct from that slot-table name: colliding the two would redefine
// the macro wherever both headers meet.
#define BLOCK_DRIVER_REQUEST_MAX 32

// pool_offset indexes the shared data pool so payloads move without copying
// through the ring. The trailing pad rounds the record to 32 bytes: a ring
// descriptor size must be a power of two (ring_resource_create), and the kernel
// binds the request ring only when its descriptor size equals this sizeof.
struct block_driver_request {
    u64 request_id;
    u32 op;
    u32 lba;
    u32 sectors;
    u32 pool_offset;
    u64 reserved;
};

struct block_driver_completion {
    u64 request_id;
    i32 status;
    u32 transferred;
};

struct block_driver_register_request {
    u32 request_ring_handle;
    u32 completion_ring_handle;
    u32 pool_handle;
    u32 sector_size;
    u32 sector_count;
    u32 flags;
    char name[BLOCK_DRIVER_ABI_NAME_MAX];
    u32 device_handle;
    u32 generation;
};

#endif
