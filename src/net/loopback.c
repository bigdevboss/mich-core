#include "loopback.h"
#include "checksum.h"
#include "net_buffer.h"

int loopback_init(struct loopback_context *loopback,
                  struct ipv4_context *ipv4, u32 buffer_count) {
    if (!loopback || !ipv4 || (ipv4->local_address >> 24) != 127 ||
        !buffer_count || buffer_count > PACKET_POOL_BUFFER_MAX)
        return -1;
    struct kernel_object *pool = packet_pool_create(buffer_count);
    if (!pool) return -1;
    loopback->ipv4 = ipv4;
    loopback->pool = pool;
    loopback->identification = 1;
    loopback->active = 1;
    loopback->stats.transmitted = 0;
    loopback->stats.received = 0;
    loopback->stats.bytes = 0;
    loopback->stats.drops = 0;
    if (ipv4_set_transmit(ipv4, loopback_transmit, loopback)) {
        object_release(pool);
        loopback->pool = 0;
        loopback->active = 0;
        return -1;
    }
    return 0;
}

void loopback_destroy(struct loopback_context *loopback) {
    if (!loopback || !loopback->active) return;
    if (loopback->pool) {
        packet_pool_revoke(loopback->pool);
        object_release(loopback->pool);
    }
    if (loopback->ipv4 && loopback->ipv4->transmit_context == loopback) {
        loopback->ipv4->transmit = 0;
        loopback->ipv4->transmit_context = 0;
    }
    loopback->ipv4 = 0;
    loopback->pool = 0;
    loopback->identification = 0;
    loopback->active = 0;
}

int loopback_transmit(struct ipv4_context *ipv4, u32 destination,
                      u8 protocol, const void *payload,
                      u32 payload_length, void *context) {
    struct loopback_context *loopback = (struct loopback_context *)context;
    if (!ipv4 || !loopback || !loopback->active || loopback->ipv4 != ipv4 ||
        (destination >> 24) != 127 || !protocol ||
        (!payload && payload_length) || payload_length > LOOPBACK_MTU) {
        if (loopback) loopback->stats.drops++;
        return -1;
    }
    u64 id = packet_pool_acquire(loopback->pool, NET_BUFFER_TX);
    u8 *packet = (u8 *)packet_pool_data(loopback->pool, id, NET_BUFFER_TX);
    if (!id || !packet) {
        loopback->stats.drops++;
        return -1;
    }
    if (ipv4_build_header(packet, NET_PACKET_DATA_MAX, payload_length,
                          ipv4->local_address, destination, protocol, 64,
                          loopback->identification++, 1)) {
        packet_pool_release(loopback->pool, id, NET_BUFFER_TX);
        loopback->stats.drops++;
        return -1;
    }
    net_copy(packet + IPV4_HEADER_MIN, payload, payload_length);
    if (packet_pool_transition(loopback->pool, id,
                               NET_BUFFER_TX, NET_BUFFER_STACK)) {
        packet_pool_release(loopback->pool, id, NET_BUFFER_TX);
        loopback->stats.drops++;
        return -1;
    }
    loopback->stats.transmitted++;
    loopback->stats.bytes += IPV4_HEADER_MIN + payload_length;
    int result = ipv4_receive_buffer(ipv4, loopback->pool, id, 0,
                                     IPV4_HEADER_MIN + payload_length);
    if (result >= 0) loopback->stats.received++;
    else loopback->stats.drops++;
    if (result != 1 &&
        packet_pool_release(loopback->pool, id, NET_BUFFER_STACK))
        return -1;
    return result < 0 ? -1 : 0;
}
