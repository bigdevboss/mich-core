#include "net_buffer.h"
#include "resource.h"

struct packet_buffer_state {
    u32 generation;
    u32 state;
};

struct packet_pool_state {
    struct kernel_object *backing;
    struct packet_buffer_state buffers[PACKET_POOL_BUFFER_MAX];
    u32 buffer_count;
    u32 next_buffer;
    u32 revoked;
    u32 active;
    // Cached physical addresses of the backing pages (one page per buffer).
    // The backing is owned by the pool (created at pool creation, released at
    // destruction), so its physical addresses are stable for the pool's whole
    // lifetime. Caching them lets packet_pool_data skip the per-call
    // page_resource_get indirection.
    paddr_t phys[PACKET_POOL_BUFFER_MAX];
};

static struct packet_pool_state pools[PACKET_POOL_MAX];
static u32 next_pool_generation;

static struct packet_pool_state *state_for(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_PACKET_POOL ||
        !object->value || object->value > PACKET_POOL_MAX)
        return 0;
    struct packet_pool_state *state = &pools[object->value - 1];
    return state->active ? state : 0;
}

static struct packet_buffer_state *buffer_for(struct packet_pool_state *pool,
                                               u64 id, u32 *slot_out) {
    u32 low = (u32)id;
    u32 generation = (u32)(id >> 32);
    if (!low || low > pool->buffer_count || !generation) return 0;
    u32 slot = low - 1;
    struct packet_buffer_state *buffer = &pool->buffers[slot];
    if (buffer->generation != generation || buffer->state == NET_BUFFER_FREE)
        return 0;
    if (slot_out) *slot_out = slot;
    return buffer;
}

static void pool_destroy(struct kernel_object *object) {
    if (!object || !object->value || object->value > PACKET_POOL_MAX) return;
    struct packet_pool_state *pool = &pools[object->value - 1];
    if (!pool->active) return;
    if (!pool->revoked) {
        struct page_resource *backing = page_resource_get(pool->backing);
        if (backing && !backing->revoked) page_resource_revoke(pool->backing);
    }
    if (pool->backing) object_release(pool->backing);
    pool->backing = 0;
    for (u32 index = 0; index < PACKET_POOL_BUFFER_MAX; index++) {
        pool->buffers[index].generation++;
        if (!pool->buffers[index].generation)
            pool->buffers[index].generation = 1;
        pool->buffers[index].state = NET_BUFFER_FREE;
    }
    pool->buffer_count = 0;
    pool->next_buffer = 0;
    pool->revoked = 0;
    pool->active = 0;
}

void packet_pool_init(void) {
    next_pool_generation = 0x10000u;
    for (u32 index = 0; index < PACKET_POOL_MAX; index++) {
        pools[index].backing = 0;
        pools[index].buffer_count = 0;
        pools[index].next_buffer = 0;
        pools[index].revoked = 0;
        pools[index].active = 0;
        for (u32 slot = 0; slot < PACKET_POOL_BUFFER_MAX; slot++) {
            pools[index].buffers[slot].generation = 1;
            pools[index].buffers[slot].state = NET_BUFFER_FREE;
        }
    }
}

struct kernel_object *packet_pool_create(u32 buffer_count) {
    if (!buffer_count || buffer_count > PACKET_POOL_BUFFER_MAX) return 0;
    for (u32 index = 0; index < PACKET_POOL_MAX; index++) {
        struct packet_pool_state *pool = &pools[index];
        if (pool->active) continue;
        struct kernel_object *backing =
            shared_memory_resource_create(buffer_count);
        if (!backing) return 0;
        pool->backing = backing;
        {
            struct page_resource *backing_res = page_resource_get(backing);
            for (u32 i = 0; i < buffer_count; i++)
                pool->phys[i] = backing_res ? backing_res->physical[i] : 0;
        }
        pool->buffer_count = buffer_count;
        pool->next_buffer = 0;
        pool->revoked = 0;
        pool->active = 1;
        u32 generation = next_pool_generation;
        next_pool_generation += 0x10000u;
        if (!next_pool_generation) next_pool_generation = 0x10000u;
        for (u32 slot = 0; slot < buffer_count; slot++) {
            pool->buffers[slot].generation = generation;
            pool->buffers[slot].state = NET_BUFFER_FREE;
        }
        struct kernel_object *object = object_create(
            KOBJECT_PACKET_POOL, index + 1, pool_destroy);
        if (!object) {
            object_release(backing);
            pool->backing = 0;
            pool->active = 0;
        }
        return object;
    }
    return 0;
}

struct kernel_object *packet_pool_backing(struct kernel_object *object) {
    struct packet_pool_state *pool = state_for(object);
    return pool && !pool->revoked ? pool->backing : 0;
}

struct packet_pool_state *packet_pool_state_for(
        const struct kernel_object *pool) {
    return state_for(pool);
}

u64 packet_pool_acquire_res(struct packet_pool_state *pool, u32 state) {
    if (!pool || pool->revoked || state == NET_BUFFER_FREE ||
        state > NET_BUFFER_DRIVER_TX)
        return 0;
    for (u32 offset = 0; offset < pool->buffer_count; offset++) {
        u32 slot = (pool->next_buffer + offset) % pool->buffer_count;
        struct packet_buffer_state *buffer = &pool->buffers[slot];
        if (buffer->state != NET_BUFFER_FREE) continue;
        buffer->state = state;
        pool->next_buffer = (slot + 1) % pool->buffer_count;
        return ((u64)buffer->generation << 32) | (slot + 1);
    }
    return 0;
}

u64 packet_pool_acquire(struct kernel_object *object, u32 state) {
    return packet_pool_acquire_res(state_for(object), state);
}

int packet_pool_transition_res(struct packet_pool_state *pool, u64 id,
                               u32 expected_state, u32 new_state) {
    struct packet_buffer_state *buffer = pool ? buffer_for(pool, id, 0) : 0;
    if (!buffer || pool->revoked || buffer->state != expected_state ||
        new_state == NET_BUFFER_FREE || new_state > NET_BUFFER_DRIVER_TX)
        return -1;
    buffer->state = new_state;
    return 0;
}

int packet_pool_transition(struct kernel_object *object, u64 id,
                           u32 expected_state, u32 new_state) {
    return packet_pool_transition_res(state_for(object), id, expected_state,
                                      new_state);
}

int packet_pool_release_res(struct packet_pool_state *pool, u64 id,
                            u32 expected_state) {
    u32 slot;
    struct packet_buffer_state *buffer = pool ? buffer_for(pool, id, &slot) : 0;
    if (!buffer || buffer->state != expected_state) return -1;
    buffer->state = NET_BUFFER_FREE;
    buffer->generation++;
    if (!buffer->generation) buffer->generation = 1;
    if (pool->next_buffer > slot) pool->next_buffer = slot;
    return 0;
}

int packet_pool_release(struct kernel_object *object, u64 id,
                        u32 expected_state) {
    return packet_pool_release_res(state_for(object), id, expected_state);
}

void *packet_pool_data_res(struct packet_pool_state *pool, u64 id,
                           u32 required_state) {
    u32 slot;
    struct packet_buffer_state *buffer = pool ? buffer_for(pool, id, &slot) : 0;
    if (!buffer || pool->revoked || buffer->state != required_state) return 0;
    // buffer_for already guarantees slot < pool->buffer_count, and the
    // backing's physical[slot] was cached into pool->phys[slot] at creation,
    // so no page_resource_get on this hot path.
    paddr_t physical = pool->phys[slot];
#if __SIZEOF_POINTER__ == 8
    if (!physical || physical >= 0x40000000ULL) return 0;
#else
    if (!physical || physical >= 0x01000000u) return 0;
#endif
    return (void *)(uptr_t)physical;
}

void *packet_pool_data(struct kernel_object *object, u64 id,
                       u32 required_state) {
    return packet_pool_data_res(state_for(object), id, required_state);
}

int packet_pool_revoke(struct kernel_object *object) {
    struct packet_pool_state *pool = state_for(object);
    if (!pool || pool->revoked) return -1;
    pool->revoked = 1;
    struct page_resource *backing = page_resource_get(pool->backing);
    int result = backing && !backing->revoked
        ? page_resource_revoke(pool->backing) : 0;
    for (u32 slot = 0; slot < pool->buffer_count; slot++) {
        pool->buffers[slot].state = NET_BUFFER_FREE;
        pool->buffers[slot].generation++;
        if (!pool->buffers[slot].generation)
            pool->buffers[slot].generation = 1;
    }
    return result;
}

u32 packet_pool_free_count(struct kernel_object *object) {
    struct packet_pool_state *pool = state_for(object);
    if (!pool || pool->revoked) return 0;
    u32 count = 0;
    for (u32 index = 0; index < pool->buffer_count; index++)
        if (pool->buffers[index].state == NET_BUFFER_FREE) count++;
    return count;
}

u32 packet_pool_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < PACKET_POOL_MAX; index++)
        if (pools[index].active) count++;
    return count;
}
