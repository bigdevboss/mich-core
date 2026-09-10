#include "vnic.h"
#include "net_buffer.h"
#include "ring.h"

void *memcpy(void *dst, const void *src, usize_t length);

struct vnic_state {
    struct kernel_object *pool;
    struct kernel_object *rx_ring;
    struct kernel_object *tx_ring;
    struct ring_resource *rx_res;
    struct ring_resource *tx_res;
    struct packet_pool_state *pool_state;
    struct vnic_stats stats;
    u32 revoked;
    u32 active;
};

static struct vnic_state vnics[VNIC_MAX];

static struct vnic_state *state_for(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_VNIC ||
        !object->value || object->value > VNIC_MAX)
        return 0;
    struct vnic_state *state = &vnics[object->value - 1];
    return state->active ? state : 0;
}

static int revoke_state(struct vnic_state *state) {
    if (!state || !state->active || state->revoked) return -1;
    state->revoked = 1;
    int result = 0;
    if (ring_resource_revoke(state->rx_ring)) result = -1;
    if (ring_resource_revoke(state->tx_ring)) result = -1;
    if (packet_pool_revoke(state->pool)) result = -1;
    return result;
}

static void vnic_destroy(struct kernel_object *object) {
    if (!object || !object->value || object->value > VNIC_MAX) return;
    struct vnic_state *state = &vnics[object->value - 1];
    if (!state->active) return;
    if (!state->revoked) revoke_state(state);
    if (state->rx_ring) object_release(state->rx_ring);
    if (state->tx_ring) object_release(state->tx_ring);
    if (state->pool) object_release(state->pool);
    state->rx_ring = 0;
    state->tx_ring = 0;
    state->pool = 0;
    state->rx_res = 0;
    state->tx_res = 0;
    state->pool_state = 0;
    state->stats.rx_packets = 0;
    state->stats.rx_drops = 0;
    state->stats.tx_packets = 0;
    state->stats.tx_drops = 0;
    state->revoked = 0;
    state->active = 0;
}

void vnic_init(void) {
    for (u32 index = 0; index < VNIC_MAX; index++) {
        vnics[index].pool = 0;
        vnics[index].rx_ring = 0;
        vnics[index].tx_ring = 0;
        vnics[index].stats.rx_packets = 0;
        vnics[index].stats.rx_drops = 0;
        vnics[index].stats.tx_packets = 0;
        vnics[index].stats.tx_drops = 0;
        vnics[index].revoked = 0;
        vnics[index].active = 0;
    }
}

struct kernel_object *vnic_create(u32 buffer_count, u32 ring_capacity) {
    if (!buffer_count || buffer_count > PACKET_POOL_BUFFER_MAX ||
        ring_capacity < 2 || ring_capacity > RING_CAPACITY_MAX)
        return 0;
    for (u32 index = 0; index < VNIC_MAX; index++) {
        struct vnic_state *state = &vnics[index];
        if (state->active) continue;
        struct kernel_object *pool = packet_pool_create(buffer_count);
        struct kernel_object *rx = pool ? ring_resource_create(
            ring_capacity, sizeof(struct net_packet_descriptor)) : 0;
        struct kernel_object *tx = rx ? ring_resource_create(
            ring_capacity, sizeof(struct net_packet_descriptor)) : 0;
        if (!pool || !rx || !tx) {
            if (tx) object_release(tx);
            if (rx) object_release(rx);
            if (pool) object_release(pool);
            return 0;
        }
        state->pool = pool;
        state->rx_ring = rx;
        state->tx_ring = tx;
        state->rx_res = ring_resource_get(rx);
        state->tx_res = ring_resource_get(tx);
        state->pool_state = packet_pool_state_for(pool);
        state->stats.rx_packets = 0;
        state->stats.rx_drops = 0;
        state->stats.tx_packets = 0;
        state->stats.tx_drops = 0;
        state->revoked = 0;
        state->active = 1;
        struct kernel_object *object = object_create(
            KOBJECT_VNIC, index + 1, vnic_destroy);
        if (!object) {
            object_release(tx);
            object_release(rx);
            object_release(pool);
            state->pool = 0;
            state->rx_ring = 0;
            state->tx_ring = 0;
            state->active = 0;
        }
        return object;
    }
    return 0;
}

struct kernel_object *vnic_pool(struct kernel_object *object) {
    struct vnic_state *state = state_for(object);
    return state && !state->revoked ? state->pool : 0;
}

struct kernel_object *vnic_rx_ring(struct kernel_object *object) {
    struct vnic_state *state = state_for(object);
    return state && !state->revoked ? state->rx_ring : 0;
}

struct kernel_object *vnic_tx_ring(struct kernel_object *object) {
    struct vnic_state *state = state_for(object);
    return state && !state->revoked ? state->tx_ring : 0;
}

int vnic_inject(struct kernel_object *object, const void *frame, u32 length) {
    struct vnic_state *state = state_for(object);
    if (!state || state->revoked || !frame || !length ||
        length > NET_PACKET_DATA_MAX - NET_PACKET_HEADROOM)
        return -1;
    u64 id = packet_pool_acquire_res(state->pool_state, NET_BUFFER_RX);
    if (!id) {
        state->stats.rx_drops++;
        return -1;
    }
    u8 *data = (u8 *)packet_pool_data_res(state->pool_state, id, NET_BUFFER_RX);
    struct ring_resource *ring = state->rx_res;
    u64 occupied = ring ? ring->producer - ring->consumer : 0;
    struct net_packet_descriptor *descriptor = ring && occupied < ring->capacity ?
        ring_resource_descriptor_res(ring, ring->producer) : 0;
    if (!data || !descriptor) {
        packet_pool_release_res(state->pool_state, id, NET_BUFFER_RX);
        state->stats.rx_drops++;
        return -1;
    }
    memcpy(data + NET_PACKET_HEADROOM, frame, length);
    descriptor->buffer_id = id;
    descriptor->offset = NET_PACKET_HEADROOM;
    descriptor->length = (u16)length;
    descriptor->flags = 0;
    descriptor->reserved0 = 0;
    descriptor->checksum = 0;
    descriptor->flow_hash = 0;
    descriptor->timestamp = 0;
    if (ring_resource_submit(state->rx_ring, 1)) {
        packet_pool_release_res(state->pool_state, id, NET_BUFFER_RX);
        state->stats.rx_drops++;
        return -1;
    }
    state->stats.rx_packets++;
    return 0;
}

int vnic_receive(struct kernel_object *object,
                 struct net_packet_descriptor *descriptor) {
    struct vnic_state *state = state_for(object);
    struct ring_resource *ring = state ? state->rx_res : 0;
    if (!state || state->revoked || !descriptor || !ring ||
        ring->producer == ring->consumer)
        return -1;
    struct net_packet_descriptor *source =
        ring_resource_descriptor_res(ring, ring->consumer);
    if (!source) return -1;
    if (source->reserved0 || !source->length ||
        source->offset > NET_PACKET_DATA_MAX ||
        source->length > NET_PACKET_DATA_MAX - source->offset ||
        packet_pool_transition_res(state->pool_state, source->buffer_id,
                                   NET_BUFFER_RX, NET_BUFFER_STACK)) {
        ring_resource_consume(state->rx_ring, 1);
        packet_pool_release_res(state->pool_state, source->buffer_id,
                                NET_BUFFER_RX);
        state->stats.rx_drops++;
        return -1;
    }
    *descriptor = *source;
    return ring_resource_consume(state->rx_ring, 1);
}

int vnic_release_rx(struct kernel_object *object, u64 buffer_id) {
    struct vnic_state *state = state_for(object);
    return state && !state->revoked ? packet_pool_release_res(
        state->pool_state, buffer_id, NET_BUFFER_STACK) : -1;
}

u64 vnic_acquire_tx(struct kernel_object *object) {
    struct vnic_state *state = state_for(object);
    return state && !state->revoked ?
        packet_pool_acquire_res(state->pool_state, NET_BUFFER_TX) : 0;
}

int vnic_submit_tx(struct kernel_object *object, u64 buffer_id,
                   u32 offset, u32 length, u32 flags) {
    struct vnic_state *state = state_for(object);
    if (!state || state->revoked || offset >= NET_PACKET_DATA_MAX || !length ||
        length > NET_PACKET_DATA_MAX - offset || length > 0xFFFFu ||
        flags > 0xFFFFu)
        return -1;
    struct ring_resource *ring = state->tx_res;
    u64 occupied = ring ? ring->producer - ring->consumer : 0;
    struct net_packet_descriptor *descriptor = ring && occupied < ring->capacity ?
        ring_resource_descriptor_res(ring, ring->producer) : 0;
    if (!descriptor || packet_pool_transition_res(
            state->pool_state, buffer_id, NET_BUFFER_TX, NET_BUFFER_TX_QUEUED)) {
        state->stats.tx_drops++;
        return -1;
    }
    descriptor->buffer_id = buffer_id;
    descriptor->offset = (u16)offset;
    descriptor->length = (u16)length;
    descriptor->flags = (u16)flags;
    descriptor->reserved0 = 0;
    descriptor->checksum = 0;
    descriptor->flow_hash = 0;
    descriptor->timestamp = 0;
    if (ring_resource_submit(state->tx_ring, 1)) {
        packet_pool_transition_res(state->pool_state, buffer_id,
                                   NET_BUFFER_TX_QUEUED, NET_BUFFER_TX);
        state->stats.tx_drops++;
        return -1;
    }
    return 0;
}

int vnic_drain_tx(struct kernel_object *object,
                  struct net_packet_descriptor *descriptor) {
    struct vnic_state *state = state_for(object);
    struct ring_resource *ring = state ? state->tx_res : 0;
    if (!state || state->revoked || !descriptor || !ring ||
        ring->producer == ring->consumer)
        return -1;
    struct net_packet_descriptor *source =
        ring_resource_descriptor_res(ring, ring->consumer);
    if (!source) return -1;
    if (source->reserved0 || !source->length ||
        source->offset >= NET_PACKET_DATA_MAX ||
        source->length > NET_PACKET_DATA_MAX - source->offset) {
        ring_resource_consume(state->tx_ring, 1);
        packet_pool_release_res(state->pool_state, source->buffer_id,
                                NET_BUFFER_TX_QUEUED);
        state->stats.tx_drops++;
        return -1;
    }
    *descriptor = *source;
    if (ring_resource_consume(state->tx_ring, 1) ||
        packet_pool_release_res(state->pool_state, source->buffer_id,
                                NET_BUFFER_TX_QUEUED))
        return -1;
    state->stats.tx_packets++;
    return 0;
}

int vnic_revoke(struct kernel_object *object) {
    return revoke_state(state_for(object));
}

int vnic_get_stats(struct kernel_object *object, struct vnic_stats *stats) {
    struct vnic_state *state = state_for(object);
    if (!state || !stats) return -1;
    *stats = state->stats;
    return 0;
}

u32 vnic_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < VNIC_MAX; index++)
        if (vnics[index].active) count++;
    return count;
}
