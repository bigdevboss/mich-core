#ifndef BLOCK_DRIVER_ABI_H
#define BLOCK_DRIVER_ABI_H

#include "types.h"

/*
 * Shared contract between the kernel block layer and a userspace block-driver
 * capsule (virtio-blk first). Mirrors net_interface_abi.h: the capsule registers
 * a data pool plus a request ring (kernel -> capsule) and a completion ring
 * (capsule -> kernel), then serves I/O over them. Freeze candidate; see
 * docs/driver-vynos.md.
 */

#define BLOCK_DRIVER_ABI_NAME_MAX 16

/* Ring depth cap: bounds outstanding requests so neither side allocates on the
 * hot path. */
#define BLOCK_REQUEST_MAX 256

/* A request payload lives at a byte offset into the shared data pool rather than
 * inline, so a batch of sectors moves without copying through the ring. */
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
    /* Filled by the kernel on success. */
    u32 device_handle;
    u32 generation;
};

#endif
