#ifndef RING_H
#define RING_H

#include "types.h"
#include "object.h"
#include "ring_abi.h"

#define RING_MAX 16
// A ring's backing is at most this many pages (see ring_resource_create,
// which bounds it by RESOURCE_PAGE_PAGES_MAX).
#define RING_BACKING_PAGES_MAX 64

struct ring_resource {
    struct kernel_object *backing;
    u64 producer;
    u64 consumer;
    u32 generation;
    u32 capacity;
    u32 descriptor_size;
    u32 data_offset;
    u32 pages;
    u32 revoked;
    u32 active;
    // Cached physical addresses of the backing pages. The backing is owned by
    // the ring (created at ring creation, released at destruction), so its
    // physical addresses are stable for the ring's whole lifetime. Caching
    // them lets the hot path (shared_header / descriptor) skip the per-op
    // page_resource_get indirection.
    paddr_t phys[RING_BACKING_PAGES_MAX];
};

void ring_init(void);
struct kernel_object *ring_resource_create(u32 capacity, u32 descriptor_size);
struct ring_resource *ring_resource_get(const struct kernel_object *object);
struct kernel_object *ring_resource_backing(struct kernel_object *object);
int ring_resource_submit(struct kernel_object *object, u32 count);
int ring_resource_consume(struct kernel_object *object, u32 count);
int ring_resource_revoke(struct kernel_object *object);
void *ring_resource_descriptor(struct kernel_object *object, u64 position);
void *ring_resource_descriptor_res(struct ring_resource *ring, u64 position);
u32 ring_active_count(void);

#endif
