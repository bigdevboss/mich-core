#include "ring.h"
#include "resource.h"

static struct ring_resource rings[RING_MAX];

static struct ring_shared_header *shared_header(struct ring_resource *ring) {
    // The backing's physical[0] was cached into ring->phys[0] at creation.
    // Callers check ring->revoked before calling this, so the cached address
    // is valid here and we avoid the page_resource_get indirection.
    return ring->phys[0] ?
        (struct ring_shared_header *)(uptr_t)ring->phys[0] : 0;
}

static void sync_header(struct ring_resource *ring) {
    struct ring_shared_header *header = shared_header(ring);
    if (!header) return;
    header->producer = ring->producer;
    header->consumer = ring->consumer;
    header->generation = ring->generation;
    header->capacity = ring->capacity;
    header->descriptor_size = ring->descriptor_size;
    header->flags = ring->revoked ? 1u : 0u;
    header->data_offset = ring->data_offset;
    header->reserved0 = 0;
    for (u32 index = 0; index < 3; index++) header->reserved[index] = 0;
}

static int revoke_ring(struct ring_resource *ring) {
    if (!ring || !ring->active || ring->revoked) return -1;
    ring->revoked = 1;
    ring->generation++;
    if (!ring->generation) ring->generation = 1;
    ring->producer = 0;
    ring->consumer = 0;
    sync_header(ring);
    struct page_resource *backing = page_resource_get(ring->backing);
    if (!backing) return -1;
    return backing->revoked ? 0 : page_resource_revoke(ring->backing);
}

static void ring_destroy(struct kernel_object *object) {
    if (!object || !object->value || object->value > RING_MAX) return;
    struct ring_resource *ring = &rings[object->value - 1];
    if (!ring->active) return;
    if (!ring->revoked) revoke_ring(ring);
    if (ring->backing) object_release(ring->backing);
    ring->backing = 0;
    ring->producer = 0;
    ring->consumer = 0;
    ring->generation = 0;
    ring->capacity = 0;
    ring->descriptor_size = 0;
    ring->data_offset = 0;
    ring->pages = 0;
    ring->revoked = 0;
    ring->active = 0;
}

void ring_init(void) {
    for (u32 index = 0; index < RING_MAX; index++) {
        rings[index].backing = 0;
        rings[index].producer = 0;
        rings[index].consumer = 0;
        rings[index].generation = 0;
        rings[index].capacity = 0;
        rings[index].descriptor_size = 0;
        rings[index].data_offset = 0;
        rings[index].pages = 0;
        rings[index].revoked = 0;
        rings[index].active = 0;
    }
}

struct kernel_object *ring_resource_create(u32 capacity, u32 descriptor_size) {
    if (capacity < 2 || capacity > RING_CAPACITY_MAX ||
        (capacity & (capacity - 1)) ||
        descriptor_size < RING_DESCRIPTOR_MIN ||
        descriptor_size > RING_DESCRIPTOR_MAX ||
        (descriptor_size & (descriptor_size - 1)))
        return 0;
    u32 data_offset = descriptor_size > RING_SHARED_HEADER_SIZE
        ? descriptor_size : RING_SHARED_HEADER_SIZE;
    u64 bytes = data_offset + (u64)capacity * descriptor_size;
    u32 pages = (u32)((bytes + 4095) / 4096);
    if (!pages || pages > RESOURCE_PAGE_PAGES_MAX) return 0;
    for (u32 index = 0; index < RING_MAX; index++) {
        struct ring_resource *ring = &rings[index];
        if (ring->active) continue;
        struct kernel_object *backing = shared_memory_resource_create(pages);
        if (!backing) return 0;
        ring->backing = backing;
        {
            struct page_resource *backing_res = page_resource_get(backing);
            for (u32 i = 0; i < pages; i++)
                ring->phys[i] = backing_res ? backing_res->physical[i] : 0;
        }
        ring->producer = 0;
        ring->consumer = 0;
        ring->generation = 1;
        ring->capacity = capacity;
        ring->descriptor_size = descriptor_size;
        ring->data_offset = data_offset;
        ring->pages = pages;
        ring->revoked = 0;
        ring->active = 1;
        struct kernel_object *object =
            object_create(KOBJECT_RING, index + 1, ring_destroy);
        if (!object) {
            object_release(backing);
            ring->backing = 0;
            ring->active = 0;
            return 0;
        }
        sync_header(ring);
        return object;
    }
    return 0;
}

struct ring_resource *ring_resource_get(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_RING ||
        !object->value || object->value > RING_MAX)
        return 0;
    struct ring_resource *ring = &rings[object->value - 1];
    return ring->active ? ring : 0;
}

struct kernel_object *ring_resource_backing(struct kernel_object *object) {
    struct ring_resource *ring = ring_resource_get(object);
    if (!ring || ring->revoked) return 0;
    return ring->backing;
}

static int rebase_if_empty(struct ring_resource *ring, u32 count) {
    if (ring->producer <= ~0ULL - count) return 0;
    if (ring->producer != ring->consumer) return -1;
    ring->producer = 0;
    ring->consumer = 0;
    return 0;
}

int ring_resource_submit(struct kernel_object *object, u32 count) {
    struct ring_resource *ring = ring_resource_get(object);
    if (!ring || ring->revoked || !count || count > ring->capacity) return -1;
    u64 occupied = ring->producer - ring->consumer;
    if (occupied > ring->capacity || count > ring->capacity - occupied ||
        rebase_if_empty(ring, count)) {
        sync_header(ring);
        return -1;
    }
    ring->producer += count;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    sync_header(ring);
    return 0;
}

int ring_resource_consume(struct kernel_object *object, u32 count) {
    struct ring_resource *ring = ring_resource_get(object);
    if (!ring || ring->revoked || !count) return -1;
    u64 available = ring->producer - ring->consumer;
    if (available > ring->capacity || count > available) {
        sync_header(ring);
        return -1;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    ring->consumer += count;
    sync_header(ring);
    return 0;
}

int ring_resource_revoke(struct kernel_object *object) {
    return revoke_ring(ring_resource_get(object));
}

void *ring_resource_descriptor_res(struct ring_resource *ring, u64 position) {
    if (!ring || ring->revoked) return 0;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    u64 slot = position & (ring->capacity - 1);
    u64 byte = ring->data_offset + slot * ring->descriptor_size;
    u32 page = (u32)(byte / 4096);
    u32 offset = (u32)(byte & 0xFFF);
    if (page >= ring->pages ||
        ring->descriptor_size > 4096 - offset)
        return 0;
    paddr_t physical = ring->phys[page];
    if (!physical) return 0;
    return (void *)(uptr_t)(physical + offset);
}

void *ring_resource_descriptor(struct kernel_object *object, u64 position) {
    return ring_resource_descriptor_res(ring_resource_get(object), position);
}

u32 ring_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < RING_MAX; index++)
        if (rings[index].active) count++;
    return count;
}
