#ifndef BLOCK_DRIVER_ABI_H
#define BLOCK_DRIVER_ABI_H

#include "types.h"

// Contract between the kernel block layer and a userspace block-driver capsule:
// a request ring (kernel -> capsule), a completion ring (capsule -> kernel), and
// a shared data pool. Mirrors net_interface_abi.h.

#define BLOCK_DRIVER_ABI_NAME_MAX 16
#define BLOCK_REQUEST_MAX 256

// pool_offset indexes the shared data pool so payloads move without copying
// through the ring.
struct block_driver_request {
    u64 request_id;
    u32 op;
    u32 lba;
    u32 sectors;
    u32 pool_offset;
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
