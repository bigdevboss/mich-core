#include "net_interface.h"
#include "checksum.h"
#include "net_buffer.h"
#include "ring.h"
#include "event.h"
#include "resource.h"
#include "socket.h"

static struct net_interface interfaces[NET_INTERFACE_MAX];
static struct tcp_context tcp_contexts[NET_INTERFACE_TCP_MAX];
static u32 tcp_context_used[NET_INTERFACE_TCP_MAX];
static struct kernel_object *registry[NET_INTERFACE_MAX];
static struct route_table *route_table;
static u32 next_interface_id;
static u64 sequence_entropy_seed = 0x4D4943484E455453ULL;

void net_interface_set_entropy(u64 entropy) {
    if (entropy) sequence_entropy_seed = entropy;
}

static u64 tcp_sequence_entropy(const struct net_interface *interface,
                                u32 address) {
    u64 value = sequence_entropy_seed ^ (u64)(uptr_t)interface;
    value ^= (u64)address << 17;
    value ^= (u64)interface->interface_id << 49;
    value ^= (u64)interface->now << 32;
    for (u32 byte = 0; byte < 6; byte++)
        value ^= (u64)interface->mac[byte] << (byte * 8);
    sequence_entropy_seed = value * 0x9E3779B97F4A7C15ULL + 1;
    return value ? value : 1;
}

static int name_valid(const char *name) {
    if (!name || !name[0]) return 0;
    for (u32 index = 0; index < NET_INTERFACE_NAME_MAX; index++)
        if (!name[index]) return 1;
    return 0;
}

static int name_equal(const char *left, const char *right) {
    for (u32 index = 0; index < NET_INTERFACE_NAME_MAX; index++) {
        if (left[index] != right[index]) return 0;
        if (!left[index]) return 1;
    }
    return 1;
}

static int mac_valid(const u8 mac[6]) {
    if (!mac || (mac[0] & 1)) return 0;
    int nonzero = 0;
    for (u32 index = 0; index < 6; index++)
        if (mac[index]) nonzero = 1;
    return nonzero;
}

static int owner_valid(const struct net_interface *interface,
                       const struct driver_domain *owner) {
    if (!interface->owner) return owner == 0;
    return owner && owner == interface->owner && owner->active &&
           owner->id == interface->owner_domain_id &&
           owner->state == DRIVER_DOMAIN_RUNNING;
}

static int netmask_valid(u32 netmask) {
    if (!netmask) return 0;
    u32 inverse = ~netmask;
    return !(inverse & (inverse + 1));
}

struct net_interface *net_interface_get(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_NET_INTERFACE ||
        !object->value || object->value > NET_INTERFACE_MAX)
        return 0;
    struct net_interface *interface = &interfaces[object->value - 1];
    return interface->active ? interface : 0;
}

static int publish_frame(struct net_interface *interface, u64 buffer_id,
                         u32 offset, u32 length) {
    struct ring_resource *ring = ring_resource_get(interface->tx_ring);
    if (!ring || ring->revoked || ring->producer - ring->consumer >= ring->capacity)
        return -1;
    struct net_packet_descriptor *descriptor =
        ring_resource_descriptor(interface->tx_ring, ring->producer);
    if (!descriptor || packet_pool_transition(interface->pool, buffer_id,
                                               NET_BUFFER_TX,
                                               NET_BUFFER_TX_QUEUED))
        return -1;
    descriptor->buffer_id = buffer_id;
    descriptor->offset = (u16)offset;
    descriptor->length = (u16)length;
    descriptor->flags = 0;
    descriptor->reserved0 = 0;
    descriptor->checksum = 0;
    descriptor->flow_hash = 0;
    descriptor->timestamp = 0;
    if (ring_resource_submit(interface->tx_ring, 1)) {
        packet_pool_transition(interface->pool, buffer_id,
                               NET_BUFFER_TX_QUEUED, NET_BUFFER_TX);
        return -1;
    }
    interface->stats.tx_packets++;
    interface->stats.tx_bytes += length;
    return 0;
}

static int transmit_arp(const u8 destination[6], u16 ether_type,
                        const void *payload, u32 length, void *context) {
    struct net_interface *interface = (struct net_interface *)context;
    if (!interface || !interface->active || interface->state != NET_INTERFACE_UP ||
        !payload || ether_type != 0x0806 || length > interface->mtu)
        return -1;
    u64 id = packet_pool_acquire(interface->pool, NET_BUFFER_TX);
    u8 *data = (u8 *)packet_pool_data(interface->pool, id, NET_BUFFER_TX);
    u32 offset = NET_PACKET_HEADROOM;
    if (!id || !data || offset + 14 + length > NET_PACKET_DATA_MAX) {
        if (id) packet_pool_release(interface->pool, id, NET_BUFFER_TX);
        return -1;
    }
    for (u32 index = 0; index < 6; index++) {
        data[offset + index] = destination[index];
        data[offset + 6 + index] = interface->mac[index];
    }
    data[offset + 12] = 0x08;
    data[offset + 13] = 0x06;
    net_copy(data + offset + 14, payload, length);
    if (publish_frame(interface, id, offset, 14 + length)) {
        packet_pool_release(interface->pool, id, NET_BUFFER_TX);
        return -1;
    }
    return 0;
}

static int flush_pending(struct net_interface *interface, u32 address,
                         const u8 hardware[6]) {
    int result = 0;
    for (u32 index = 0; index < NET_INTERFACE_PENDING_MAX; index++) {
        struct net_interface_pending *pending = &interface->pending[index];
        if (!pending->active || pending->next_hop != address) continue;
        u8 *data = (u8 *)packet_pool_data(
            interface->pool, pending->buffer_id, NET_BUFFER_TX);
        if (!data || pending->offset < 14) {
            if (packet_pool_release(interface->pool, pending->buffer_id,
                                    NET_BUFFER_TX))
                result = -1;
            pending->active = 0;
            if (interface->pending_count) interface->pending_count--;
            result = -1;
            continue;
        }
        u32 frame_offset = pending->offset - 14;
        for (u32 byte = 0; byte < 6; byte++) {
            data[frame_offset + byte] = hardware[byte];
            data[frame_offset + 6 + byte] = interface->mac[byte];
        }
        data[frame_offset + 12] = 0x08;
        data[frame_offset + 13] = 0x00;
        if (publish_frame(interface, pending->buffer_id, frame_offset,
                          pending->length + 14)) {
            result = -1;
            continue;
        }
        pending->active = 0;
        if (interface->pending_count) interface->pending_count--;
        interface->stats.arp_flushed++;
    }
    return result;
}

static void neighbor_updated(u32 address, const u8 hardware[6], void *context) {
    struct net_interface *interface = (struct net_interface *)context;
    if (interface && interface->active && interface->state == NET_INTERFACE_UP)
        flush_pending(interface, address, hardware);
}

static int revoke_interface(struct net_interface *interface) {
    if (!interface || !interface->active ||
        interface->state == NET_INTERFACE_REVOKED ||
        interface->state == NET_INTERFACE_REMOVED)
        return -1;
    u32 old_generation = interface->generation;
    interface->state = NET_INTERFACE_QUIESCING;
    event_signal(interface->event);
    int result = 0;
    if (interface->udp_ready) socket_unregister_udp(&interface->udp);
    if (interface->ipv6_state == NET_INTERFACE_IPV6_SLAAC)
        socket_unregister_udpv6(&interface->udpv6);
    if (interface->udp_probe_binding) {
        if (udp_unbind(&interface->udp, interface->udp_probe_binding))
            result = -1;
        interface->udp_probe_binding = 0;
    }
    if (interface->udpv6_probe_binding) {
        if (udpv6_unbind(&interface->udpv6,
                         interface->udpv6_probe_binding))
            result = -1;
        interface->udpv6_probe_binding = 0;
    }
    if (interface->tcp) socket_tcp_abort_context(interface->tcp, -19);
    if (interface->tcp_probe_connection) {
        if (tcp_close(interface->tcp, interface->tcp_probe_connection))
            result = -1;
        interface->tcp_probe_connection = 0;
    }
    if (interface->tcpv6_probe_connection) {
        if (tcp_close(interface->tcp, interface->tcpv6_probe_connection))
            result = -1;
        interface->tcpv6_probe_connection = 0;
    }
    for (u32 index = 0; index < NET_INTERFACE_PENDING_MAX; index++) {
        struct net_interface_pending *pending = &interface->pending[index];
        if (!pending->active) continue;
        if (packet_pool_release(interface->pool, pending->buffer_id,
                                NET_BUFFER_TX))
            result = -1;
        pending->active = 0;
    }
    interface->pending_count = 0;
    struct ring_resource *rx = ring_resource_get(interface->rx_ring);
    struct ring_resource *tx = ring_resource_get(interface->tx_ring);
    struct page_resource *pool = page_resource_get(
        packet_pool_backing(interface->pool));
    if (rx && !rx->revoked && ring_resource_revoke(interface->rx_ring)) result = -1;
    if (tx && !tx->revoked && ring_resource_revoke(interface->tx_ring)) result = -1;
    if (pool && !pool->revoked && packet_pool_revoke(interface->pool)) result = -1;
    if (route_table && interface->interface_id)
        route_deactivate_interface(route_table, interface->interface_id,
                                   old_generation);
    interface->generation++;
    if (!interface->generation) interface->generation = 1;
    interface->state = NET_INTERFACE_REVOKED;
    event_signal(interface->event);
    return result;
}

static void interface_destroy(struct kernel_object *object) {
    if (!object || !object->value || object->value > NET_INTERFACE_MAX) return;
    struct net_interface *interface = &interfaces[object->value - 1];
    if (!interface->active) return;
    if (interface->state != NET_INTERFACE_REVOKED &&
        interface->state != NET_INTERFACE_REMOVED)
        revoke_interface(interface);
    if (interface->event) object_release(interface->event);
    if (interface->rx_ring) object_release(interface->rx_ring);
    if (interface->tx_ring) object_release(interface->tx_ring);
    if (interface->pool) object_release(interface->pool);
    interface->event = 0;
    interface->rx_ring = 0;
    interface->tx_ring = 0;
    interface->pool = 0;
    interface->owner = 0;
    interface->self = 0;
    interface->pending_count = 0;
    interface->arp_ready = 0;
    interface->ipv4_address = 0;
    interface->ipv4_netmask = 0;
    interface->ipv4_gateway = 0;
    interface->ipv4_ready = 0;
    interface->udp_ready = 0;
    interface->udp_probe_complete = 0;
    interface->udp_probe_binding = 0;
    interface->tcp_probe_connection = 0;
    if (interface->tcp_slot < NET_INTERFACE_TCP_MAX)
        tcp_context_used[interface->tcp_slot] = 0;
    interface->tcp = 0;
    interface->tcp_slot = NET_INTERFACE_TCP_MAX;
    interface->tcp_probe_sent = 0;
    interface->tcp_probe_complete = 0;
    interface->tcpv6_probe_connection = 0;
    interface->tcpv6_probe_sent = 0;
    interface->tcpv6_probe_complete = 0;
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++) {
        interface->ipv6_link_local[index] = 0;
        interface->ipv6_global[index] = 0;
        interface->ipv6_router[index] = 0;
    }
    interface->ipv6_state = NET_INTERFACE_IPV6_DISABLED;
    interface->ipv6_deprecated = 0;
    interface->ipv6_rs_retries = 0;
    interface->ipv6_rs_deadline = 0;
    interface->ipv6_echo_replies = 0;
    interface->udpv6_probe_binding = 0;
    interface->udpv6_probe_complete = 0;
    interface->owner_domain_id = 0;
    interface->interface_id = 0;
    interface->state = NET_INTERFACE_REMOVED;
    interface->registered = 0;
    interface->active = 0;
}

int net_interface_init(struct route_table *routes) {
    if (!routes) return -1;
    for (u32 index = 0; index < NET_INTERFACE_MAX; index++)
        if (interfaces[index].active || registry[index]) return -1;
    route_table = routes;
    next_interface_id = 1;
    for (u32 index = 0; index < NET_INTERFACE_TCP_MAX; index++)
        tcp_context_used[index] = 0;
    for (u32 index = 0; index < NET_INTERFACE_MAX; index++) {
        registry[index] = 0;
        interfaces[index].pool = 0;
        interfaces[index].rx_ring = 0;
        interfaces[index].tx_ring = 0;
        interfaces[index].event = 0;
        interfaces[index].owner = 0;
        interfaces[index].self = 0;
        interfaces[index].pending_count = 0;
        interfaces[index].now = 0;
        interfaces[index].arp_ready = 0;
        interfaces[index].owner_domain_id = 0;
        interfaces[index].interface_id = 0;
        interfaces[index].generation = 1;
        interfaces[index].state = NET_INTERFACE_REMOVED;
        interfaces[index].registered = 0;
        interfaces[index].active = 0;
    }
    return 0;
}

struct kernel_object *net_interface_create(
    struct driver_domain *owner, struct kernel_object *pool,
    struct kernel_object *rx_ring, struct kernel_object *tx_ring,
    const u8 mac[6], u32 mtu, const char *name) {
    if (!packet_pool_backing(pool) || !ring_resource_get(rx_ring) ||
        !ring_resource_get(tx_ring) || !mac_valid(mac) || !name_valid(name) ||
        mtu < 576 || mtu > NET_PACKET_DATA_MAX - NET_PACKET_HEADROOM ||
        (owner && (!owner->active || owner->state != DRIVER_DOMAIN_RUNNING)))
        return 0;
    for (u32 index = 0; index < NET_INTERFACE_MAX; index++) {
        struct net_interface *interface = &interfaces[index];
        if (interface->active) continue;
        if (ethernet_port_init(&interface->ethernet, mac)) return 0;
        struct kernel_object *event = event_create(EVENT_AUTO_RESET, 0);
        if (!event) return 0;
        if (object_retain(pool)) {
            object_release(event);
            return 0;
        }
        if (object_retain(rx_ring)) {
            object_release(pool);
            object_release(event);
            return 0;
        }
        if (object_retain(tx_ring)) {
            object_release(rx_ring);
            object_release(pool);
            object_release(event);
            return 0;
        }
        interface->pool = pool;
        interface->rx_ring = rx_ring;
        interface->tx_ring = tx_ring;
        interface->event = event;
        interface->owner = owner;
        interface->self = 0;
        interface->pending_count = 0;
        interface->now = 0;
        interface->last_arp_tick = 0;
        interface->arp_ready = 0;
        for (u32 pending = 0; pending < NET_INTERFACE_PENDING_MAX; pending++) {
            interface->pending[pending].buffer_id = 0;
            interface->pending[pending].destination = 0;
            interface->pending[pending].next_hop = 0;
            interface->pending[pending].expires = 0;
            interface->pending[pending].offset = 0;
            interface->pending[pending].length = 0;
            interface->pending[pending].active = 0;
        }
        interface->owner_domain_id = owner ? owner->id : 0;
        interface->interface_id = 0;
        interface->state = NET_INTERFACE_CREATED;
        interface->mtu = mtu;
        interface->ipv4_address = 0;
        interface->ipv4_netmask = 0;
        interface->ipv4_gateway = 0;
        interface->ipv4_ready = 0;
        interface->udp_ready = 0;
        interface->udp_probe_complete = 0;
        interface->udp_probe_binding = 0;
        interface->tcp_probe_connection = 0;
        interface->tcp = 0;
        interface->tcp_slot = NET_INTERFACE_TCP_MAX;
        interface->tcp_probe_sent = 0;
        interface->tcp_probe_complete = 0;
        interface->tcpv6_probe_connection = 0;
        interface->tcpv6_probe_sent = 0;
        interface->tcpv6_probe_complete = 0;
        for (u32 address = 0; address < IPV6_ADDRESS_SIZE; address++) {
            interface->ipv6_link_local[address] = 0;
            interface->ipv6_global[address] = 0;
            interface->ipv6_router[address] = 0;
        }
        interface->ipv6_state = NET_INTERFACE_IPV6_DISABLED;
        interface->ipv6_deprecated = 0;
        interface->ipv6_rs_retries = 0;
        interface->ipv6_rs_deadline = 0;
        interface->ipv6_echo_replies = 0;
        interface->udpv6_probe_binding = 0;
        interface->udpv6_probe_complete = 0;
        interface->registered = 0;
        for (u32 byte = 0; byte < 6; byte++) interface->mac[byte] = mac[byte];
        for (u32 byte = 0; byte < NET_INTERFACE_NAME_MAX; byte++)
            interface->name[byte] = 0;
        for (u32 byte = 0; name[byte]; byte++) interface->name[byte] = name[byte];
        u8 *stats = (u8 *)&interface->stats;
        for (usize_t byte = 0; byte < sizeof(interface->stats); byte++)
            stats[byte] = 0;
        pmtu_init(&interface->pmtu, 60000);
        interface->active = 1;
        struct kernel_object *object = object_create(
            KOBJECT_NET_INTERFACE, index + 1, interface_destroy);
        if (object) interface->self = object;
        if (!object) {
            object_release(event);
            object_release(rx_ring);
            object_release(tx_ring);
            object_release(pool);
            interface->active = 0;
        }
        return object;
    }
    return 0;
}

int net_interface_register(struct kernel_object *object) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || interface->registered ||
        interface->state != NET_INTERFACE_CREATED)
        return -1;
    for (u32 index = 0; index < NET_INTERFACE_MAX; index++)
        if (registry[index]) {
            struct net_interface *other = net_interface_get(registry[index]);
            if (other && name_equal(other->name, interface->name)) return -1;
        }
    u32 slot = NET_INTERFACE_MAX;
    for (u32 index = 0; index < NET_INTERFACE_MAX; index++)
        if (!registry[index]) {
            slot = index;
            break;
        }
    if (slot == NET_INTERFACE_MAX || object_retain(object)) return -1;
    interface->interface_id = next_interface_id++;
    if (!next_interface_id) next_interface_id = 1;
    interface->registered = 1;
    interface->state = NET_INTERFACE_DOWN;
    registry[slot] = object;
    event_signal(interface->event);
    return 0;
}

int net_interface_set_link(struct kernel_object *object,
                           struct driver_domain *owner, int up) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !interface->registered || !owner_valid(interface, owner) ||
        (interface->state != NET_INTERFACE_DOWN &&
         interface->state != NET_INTERFACE_UP))
        return -1;
    interface->state = up ? NET_INTERFACE_UP : NET_INTERFACE_DOWN;
    event_signal(interface->event);
    return 0;
}

static int ipv6_tx_reserve(
    struct net_interface *interface,
    const u8 source[IPV6_ADDRESS_SIZE],
    const u8 destination[IPV6_ADDRESS_SIZE],
    u8 next_header, u32 payload_length,
    u64 *id_out, u32 *frame_offset_out, u8 **payload_out, u32 *capacity_out) {
    u32 limit = interface ?
        pmtu_lookup6(&interface->pmtu, destination, interface->mtu) : 0;
    if (!interface || !source || !destination ||
        limit < IPV6_HEADER_SIZE ||
        payload_length > limit - IPV6_HEADER_SIZE) {
        if (interface) interface->stats.mtu_drops++;
        return -1;
    }
    u8 destination_mac[6];
    if (ipv6_address_multicast(destination)) {
        destination_mac[0] = 0x33;
        destination_mac[1] = 0x33;
        destination_mac[2] = destination[12];
        destination_mac[3] = destination[13];
        destination_mac[4] = destination[14];
        destination_mac[5] = destination[15];
    } else if (icmpv6_neighbor_lookup(
                   &interface->icmpv6, destination, destination_mac) &&
               (ipv6_address_unspecified(interface->ipv6_router) ||
                icmpv6_neighbor_lookup(
                    &interface->icmpv6, interface->ipv6_router,
                    destination_mac))) {
        return -1;
    }
    u64 id = packet_pool_acquire(interface->pool, NET_BUFFER_TX);
    u8 *data = (u8 *)packet_pool_data(interface->pool, id, NET_BUFFER_TX);
    u32 offset = NET_PACKET_HEADROOM;
    if (!id || !data || ipv6_build_header(
            data + offset, NET_PACKET_DATA_MAX - offset,
            payload_length, source, destination, next_header, 255, 0, 0)) {
        if (id) packet_pool_release(interface->pool, id, NET_BUFFER_TX);
        return -1;
    }
    u32 frame_offset = offset - 14;
    for (u32 index = 0; index < 6; index++) {
        data[frame_offset + index] = destination_mac[index];
        data[frame_offset + 6 + index] = interface->mac[index];
    }
    data[frame_offset + 12] = 0x86;
    data[frame_offset + 13] = 0xDD;
    *id_out = id;
    *frame_offset_out = frame_offset;
    *payload_out = data + offset + IPV6_HEADER_SIZE;
    *capacity_out = NET_PACKET_DATA_MAX - offset - IPV6_HEADER_SIZE;
    return 0;
}

static int ipv6_tx_commit(struct net_interface *interface, u64 id,
                          u32 frame_offset, u32 payload_length) {
    if (publish_frame(interface, id, frame_offset,
                      14 + IPV6_HEADER_SIZE + payload_length)) {
        packet_pool_release(interface->pool, id, NET_BUFFER_TX);
        return -1;
    }
    return 0;
}

static int transmit_ipv6_packet(
    struct net_interface *interface,
    const u8 source[IPV6_ADDRESS_SIZE],
    const u8 destination[IPV6_ADDRESS_SIZE],
    u8 next_header, const void *payload, u32 payload_length) {
    u64 id;
    u32 frame_offset;
    u8 *out;
    u32 capacity;
    if (!payload || ipv6_tx_reserve(interface, source, destination,
                                    next_header, payload_length, &id,
                                    &frame_offset, &out, &capacity)) {
        if (!payload && interface) interface->stats.mtu_drops++;
        return -1;
    }
    if (payload_length > capacity) {
        packet_pool_release(interface->pool, id, NET_BUFFER_TX);
        return -1;
    }
    net_copy(out, payload, payload_length);
    return ipv6_tx_commit(interface, id, frame_offset, payload_length);
}

static int transmit_icmpv6(
    const u8 source[IPV6_ADDRESS_SIZE],
    const u8 destination[IPV6_ADDRESS_SIZE],
    const void *message, u32 length, void *context) {
    struct net_interface *interface = context;
    return transmit_ipv6_packet(
        interface, source, destination, 58, message, length);
}

static int transmit_udpv6(
    const u8 source[IPV6_ADDRESS_SIZE],
    const u8 destination[IPV6_ADDRESS_SIZE],
    const void *datagram, u32 length, void *context) {
    return transmit_ipv6_packet(
        context, source, destination, 17, datagram, length);
}

static int ipv4_tx_reserve(struct net_interface *interface, u32 destination,
                           u8 protocol, u32 payload_length,
                           u64 *id_out, u32 *offset_out, u8 **payload_out,
                           u32 *capacity_out) {
    u32 limit = interface ?
        pmtu_lookup4(&interface->pmtu, destination, interface->mtu) : 0;
    if (!interface || limit < IPV4_HEADER_MIN ||
        payload_length > limit - IPV4_HEADER_MIN) {
        if (interface) interface->stats.mtu_drops++;
        return -1;
    }
    u64 id = packet_pool_acquire(interface->pool, NET_BUFFER_TX);
    u8 *data = (u8 *)packet_pool_data(interface->pool, id, NET_BUFFER_TX);
    u32 offset = NET_PACKET_HEADROOM;
    if (!id || !data || ipv4_build_header(
            data + offset, NET_PACKET_DATA_MAX - offset, payload_length,
            interface->ipv4_address, destination, protocol, 64,
            (u16)interface->stats.tx_packets, 1)) {
        if (id) packet_pool_release(interface->pool, id, NET_BUFFER_TX);
        return -1;
    }
    *id_out = id;
    *offset_out = offset;
    *payload_out = data + offset + IPV4_HEADER_MIN;
    *capacity_out = NET_PACKET_DATA_MAX - offset - IPV4_HEADER_MIN;
    return 0;
}

static int ipv4_tx_commit(struct net_interface *interface, u64 id, u32 offset,
                          u32 destination, u32 payload_length) {
    u32 next_hop = (destination & interface->ipv4_netmask) ==
                   (interface->ipv4_address & interface->ipv4_netmask) ?
                   destination : interface->ipv4_gateway;
    int result = net_interface_send_ipv4(
        interface->self, id, offset, IPV4_HEADER_MIN + payload_length,
        destination, next_hop, interface->now);
    if (result < 0) packet_pool_release(interface->pool, id, NET_BUFFER_TX);
    return result;
}

static int transmit_tcp(struct net_interface *interface,
                        const struct tcp_transmit *transmit) {
    if (!interface || !transmit ||
        transmit->length > TCP_RETRANSMIT_DATA_MAX ||
        transmit->option_length > TCP_OPTION_MAX ||
        transmit->option_length % 4)
        return -1;
    u32 tcp_length = TCP_HEADER_MIN + transmit->option_length + transmit->length;
    const void *payload = transmit->length ? transmit->data : 0;
    u64 id;
    u32 offset;
    u8 *out;
    u32 capacity;
    int length;
    if (transmit->family == 6) {
        if (ipv6_tx_reserve(interface, transmit->source_address6,
                            transmit->destination_address6, 6, tcp_length,
                            &id, &offset, &out, &capacity))
            return -1;
        length = tcp_build_ipv6_opts(
            out, capacity, transmit->source_address6,
            transmit->destination_address6, transmit->source_port,
            transmit->destination_port, transmit->sequence,
            transmit->acknowledgement, transmit->flags,
            transmit->window, transmit->options, transmit->option_length,
            payload, transmit->length);
        if (length < 0 || (u32)length != tcp_length) {
            packet_pool_release(interface->pool, id, NET_BUFFER_TX);
            return -1;
        }
        return ipv6_tx_commit(interface, id, offset, tcp_length);
    }
    if (ipv4_tx_reserve(interface, transmit->destination_address, 6,
                        tcp_length, &id, &offset, &out, &capacity))
        return -1;
    length = tcp_build_ipv4_opts(
        out, capacity, transmit->source_address,
        transmit->destination_address, transmit->source_port,
        transmit->destination_port, transmit->sequence,
        transmit->acknowledgement, transmit->flags,
        transmit->window, transmit->options, transmit->option_length,
        payload, transmit->length);
    if (length < 0 || (u32)length != tcp_length) {
        packet_pool_release(interface->pool, id, NET_BUFFER_TX);
        return -1;
    }
    return ipv4_tx_commit(interface, id, offset,
                          transmit->destination_address, tcp_length);
}

static int flush_tcp(struct net_interface *interface, u64 connection) {
    if (!interface || !interface->tcp || !connection) return -1;
    for (u32 budget = 0; budget < 8; budget++) {
        struct tcp_transmit transmit;
        int ready = tcp_prepare_transmit(
            interface->tcp, connection, interface->now, &transmit);
        if (ready <= 0) break;
        if (transmit_tcp(interface, &transmit) < 0) return -1;
    }
    return 0;
}

// Responses carry either SYN options or SACK blocks; both travel as
// the transmit's option string.
static void transmit_from_response(struct tcp_transmit *transmit,
                                   const struct tcp_response *response) {
    transmit->option_length = 0;
    if (response->option_length &&
        response->option_length <= sizeof(transmit->options)) {
        for (u32 index = 0; index < response->option_length; index++)
            transmit->options[index] = response->options[index];
        transmit->option_length = response->option_length;
    } else if (response->sack_length &&
               response->sack_length <= sizeof(transmit->options)) {
        for (u32 index = 0; index < response->sack_length; index++)
            transmit->options[index] = response->sack[index];
        transmit->option_length = response->sack_length;
    }
}

static int interface_tcp_handler(
    struct ipv4_context *ipv4,
    const struct ipv4_packet_view *packet, void *context) {
    struct net_interface *interface = context;
    if (!interface || ipv4 != &interface->ipv4 || !packet ||
        packet->protocol != 6)
        return -1;
    struct tcp_response response;
    int result = tcp_receive_ipv4(
        interface->tcp, packet->source, packet->destination,
        packet->payload, packet->payload_length, &response);
    if (result < 0 && !response.valid) return -1;
    if (response.connection_id)
        socket_tcp_notify(interface->tcp, response.connection_id);
    if (response.valid) {
        // Reject responses to non-unicast source addresses to avoid
        // broadcast or multicast amplification.
        u8 first = (u8)(packet->source >> 24);
        if (first == 0 || first >= 224) return -1;
        struct tcp_transmit transmit;
        transmit.connection_id = 0;
        transmit.family = 4;
        transmit.source_address = packet->destination;
        transmit.destination_address = packet->source;
        transmit.sequence = response.sequence;
        transmit.acknowledgement = response.acknowledgement;
        transmit.source_port = response.source_port;
        transmit.destination_port = response.destination_port;
        transmit.window = response.window;
        transmit.length = 0;
        transmit.flags = response.flags;
        transmit.retransmission = 0;
        transmit_from_response(&transmit, &response);
        if (transmit_tcp(interface, &transmit) < 0) return -1;
    }
    if (response.connection_id &&
        flush_tcp(interface, response.connection_id) < 0)
        return -1;
    return 0;
}

static int interface_tcpv6_handler(
    struct ipv6_context *ipv6,
    const struct ipv6_packet_view *packet, void *context) {
    struct net_interface *interface = context;
    if (!interface || ipv6 != &interface->ipv6 || !interface->tcp ||
        !packet || packet->next_header != 6)
        return -1;
    struct tcp_response response;
    int result = tcp_receive_ipv6(
        interface->tcp, packet->source, packet->destination,
        packet->payload, packet->payload_length, &response);
    if (result < 0 && !response.valid) return -1;
    if (response.connection_id)
        socket_tcp_notify(interface->tcp, response.connection_id);
    if (response.valid) {
        // Reject responses to multicast sources.
        if (packet->source[0] == 0xFF) return -1;
        struct tcp_transmit transmit;
        transmit.connection_id = response.connection_id;
        transmit.family = 6;
        transmit.source_address = 0;
        transmit.destination_address = 0;
        for (u32 byte = 0; byte < 16; byte++) {
            transmit.source_address6[byte] = packet->destination[byte];
            transmit.destination_address6[byte] = packet->source[byte];
        }
        transmit.sequence = response.sequence;
        transmit.acknowledgement = response.acknowledgement;
        transmit.source_port = response.source_port;
        transmit.destination_port = response.destination_port;
        transmit.window = response.window;
        transmit.length = 0;
        transmit.flags = response.flags;
        transmit.retransmission = 0;
        transmit_from_response(&transmit, &response);
        if (transmit_tcp(interface, &transmit) < 0) return -1;
    }
    if (response.connection_id &&
        flush_tcp(interface, response.connection_id) < 0)
        return -1;
    return 0;
}

static int interface_udp_unreachable(
    const struct ipv4_packet_view *packet, void *context) {
    return icmp_send_port_unreachable((struct icmp_context *)context, packet);
}

static void interface_pmtu4(u32 destination, u32 mtu, void *context) {
    struct net_interface *interface = context;
    if (!interface) return;
    if (pmtu_update4(&interface->pmtu, destination, mtu, interface->mtu))
        return;
    if (interface->tcp)
        tcp_clamp_pmtu(interface->tcp, 4, destination, 0,
                       pmtu_lookup4(&interface->pmtu, destination,
                                    interface->mtu));
}

static void interface_pmtu6(const u8 destination[IPV6_ADDRESS_SIZE],
                            u32 mtu, void *context) {
    struct net_interface *interface = context;
    if (!interface || !destination) return;
    if (pmtu_update6(&interface->pmtu, destination, mtu, interface->mtu))
        return;
    if (interface->tcp)
        tcp_clamp_pmtu(interface->tcp, 6, 0, destination,
                       pmtu_lookup6(&interface->pmtu, destination,
                                    interface->mtu));
}

static void interface_tcp_blackhole(u32 family, u32 address4,
                                    const u8 address6[16], u32 mtu,
                                    void *context) {
    if (family == 4)
        interface_pmtu4(address4, mtu, context);
    else
        interface_pmtu6(address6, mtu, context);
}

static int transmit_ipv4(struct ipv4_context *ipv4, u32 destination,
                         u8 protocol, const void *payload,
                         u32 payload_length, void *context) {
    struct net_interface *interface = context;
    u64 id;
    u32 offset;
    u8 *out;
    u32 capacity;
    if (!interface || !ipv4 || ipv4 != &interface->ipv4 || !payload) {
        if (interface) interface->stats.mtu_drops++;
        return -1;
    }
    if (ipv4_tx_reserve(interface, destination, protocol, payload_length,
                        &id, &offset, &out, &capacity))
        return -1;
    if (payload_length > capacity) {
        packet_pool_release(interface->pool, id, NET_BUFFER_TX);
        return -1;
    }
    net_copy(out, payload, payload_length);
    return ipv4_tx_commit(interface, id, offset, destination, payload_length);
}

int net_interface_set_ipv4(struct kernel_object *object,
                           struct driver_domain *owner,
                           u32 address, u32 netmask) {
    struct net_interface *interface = net_interface_get(object);
    u8 first = (u8)(address >> 24);
    if (!interface || !interface->registered || !owner_valid(interface, owner) ||
        !address || address == 0xFFFFFFFFu || first == 0 || first >= 224 ||
        (owner && first == 127) || !netmask_valid(netmask))
        return -1;
    if (!interface->arp_ready) {
        if (arp_init(&interface->arp, &interface->ethernet, address,
                     300, 10, transmit_arp, interface) ||
            arp_set_update_callback(&interface->arp,
                                    neighbor_updated, interface))
            return -1;
        interface->arp_ready = 1;
    } else if (arp_set_local_address(&interface->arp, address)) {
        return -1;
    }
    if (!interface->ipv4_ready) {
        if (ipv4_init(&interface->ipv4, &interface->ethernet,
                      address, netmask, 1) ||
            ipv4_set_transmit(&interface->ipv4, transmit_ipv4, interface) ||
            icmp_init(&interface->icmp, &interface->ipv4, 100, 64) ||
            icmp_set_pmtu_callback(&interface->icmp, interface_pmtu4,
                                   interface) ||
            udp_init(&interface->udp, &interface->ipv4) ||
            udp_set_unreachable_callback(
                &interface->udp, interface_udp_unreachable,
                &interface->icmp))
            return -1;
        u32 tcp_slot = NET_INTERFACE_TCP_MAX;
        for (u32 index = 0; index < NET_INTERFACE_TCP_MAX; index++)
            if (!tcp_context_used[index]) {
                tcp_slot = index;
                break;
            }
        if (tcp_slot == NET_INTERFACE_TCP_MAX) return -1;
        tcp_context_used[tcp_slot] = 1;
        interface->tcp_slot = tcp_slot;
        interface->tcp = &tcp_contexts[tcp_slot];
        tcp_init(interface->tcp,
                 tcp_sequence_entropy(interface, address));
        // tcp_set_mss caps at the retransmit/OOO payload, not the MTU.
        tcp_set_mss(interface->tcp, (u16)(interface->mtu - 40));
        tcp_set_pmtu_blackhole_callback(interface->tcp,
                                        interface_tcp_blackhole,
                                        interface);
        if (ipv4_register_protocol(&interface->ipv4, 6,
                                   interface_tcp_handler, interface)) {
            tcp_context_used[tcp_slot] = 0;
            interface->tcp = 0;
            interface->tcp_slot = NET_INTERFACE_TCP_MAX;
            return -1;
        }
        interface->ipv4_ready = 1;
        interface->udp_ready = 1;
    }
    interface->ipv4_address = address;
    interface->ipv4_netmask = netmask;
    event_signal(interface->event);
    return 0;
}

int net_interface_configure_ipv4(struct kernel_object *object,
                                 struct driver_domain *owner,
                                 u32 address, u32 netmask, u32 gateway) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !route_table || route_available(route_table) < 2 ||
        interface->ipv4_address || !gateway || gateway == address ||
        (gateway & netmask) != (address & netmask) ||
        net_interface_set_ipv4(object, owner, address, netmask))
        return -1;
    u32 network = address & netmask;
    if (route_add_generation(route_table, network, netmask, 0,
                             interface->interface_id,
                             interface->generation, 10) ||
        route_add_generation(route_table, 0, 0, gateway,
                             interface->interface_id,
                             interface->generation, 100) ||
        socket_register_udp(&interface->udp, interface->interface_id,
                            interface->generation)) {
        route_deactivate_interface(route_table, interface->interface_id,
                                   interface->generation);
        interface->ipv4_address = 0;
        interface->ipv4_netmask = 0;
        return -1;
    }
    interface->ipv4_gateway = gateway;
    event_signal(interface->event);
    return 0;
}

int net_interface_send_echo(struct kernel_object *object,
                            struct driver_domain *owner, u32 destination,
                            u16 identifier, u16 sequence) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !owner_valid(interface, owner) ||
        !interface->ipv4_ready || !destination)
        return -1;
    u8 message[32];
    for (u32 index = 0; index < sizeof(message); index++) message[index] = 0;
    message[0] = ICMP_ECHO_REQUEST;
    message[4] = (u8)(identifier >> 8);
    message[5] = (u8)identifier;
    message[6] = (u8)(sequence >> 8);
    message[7] = (u8)sequence;
    for (u32 index = ICMP_HEADER_SIZE; index < sizeof(message); index++)
        message[index] = (u8)index;
    u16 checksum = icmp_checksum(message, sizeof(message));
    message[2] = (u8)(checksum >> 8);
    message[3] = (u8)checksum;
    return ipv4_send(&interface->ipv4, destination, 1,
                     message, sizeof(message));
}

u64 net_interface_echo_replies(struct kernel_object *object,
                               struct driver_domain *owner) {
    struct net_interface *interface = net_interface_get(object);
    return interface && owner_valid(interface, owner) && interface->ipv4_ready ?
        interface->icmp.stats.echo_replies : 0;
}

int net_interface_send_udp_probe(struct kernel_object *object,
                                 struct driver_domain *owner,
                                 u32 destination) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !owner_valid(interface, owner) ||
        !interface->udp_ready || interface->udp_probe_binding ||
        !destination)
        return -1;
    u64 binding = udp_bind(&interface->udp, interface->ipv4_address, 0);
    if (!binding) return -1;
    u8 query[29];
    for (u32 index = 0; index < sizeof(query); index++) query[index] = 0;
    query[0] = 0x4D;
    query[1] = 0x49;
    query[2] = 0x01;
    query[5] = 1;
    query[12] = 7;
    query[13] = 'e';
    query[14] = 'x';
    query[15] = 'a';
    query[16] = 'm';
    query[17] = 'p';
    query[18] = 'l';
    query[19] = 'e';
    query[20] = 3;
    query[21] = 'c';
    query[22] = 'o';
    query[23] = 'm';
    query[25] = 0;
    query[26] = 1;
    query[27] = 0;
    query[28] = 1;
    if (udp_send(&interface->udp, binding, destination, 53,
                 query, sizeof(query))) {
        udp_unbind(&interface->udp, binding);
        return -1;
    }
    interface->udp_probe_binding = binding;
    interface->udp_probe_complete = 0;
    return 0;
}

int net_interface_poll_udp_probe(struct kernel_object *object,
                                 struct driver_domain *owner) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !owner_valid(interface, owner) ||
        !interface->udp_probe_binding)
        return -1;
    struct udp_datagram datagram;
    if (udp_receive(&interface->udp, interface->udp_probe_binding, &datagram))
        return 0;
    int valid = datagram.source_port == 53 && datagram.length >= 12 &&
        datagram.payload[0] == 0x4D && datagram.payload[1] == 0x49 &&
        (datagram.payload[2] & 0x80);
    if (udp_unbind(&interface->udp, interface->udp_probe_binding))
        return -1;
    interface->udp_probe_binding = 0;
    interface->udp_probe_complete = valid != 0;
    return valid ? 1 : -1;
}

int net_interface_tcp_probe_start(struct kernel_object *object,
                                  struct driver_domain *owner,
                                  u32 destination, u16 port) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !owner_valid(interface, owner) ||
        !interface->ipv4_ready || interface->tcp_probe_connection ||
        !destination || !port)
        return -1;
    // Mix next_sequence into the local port so two interfaces with
    // the same interface_id low byte do not collide.
    u16 local_port = (u16)(55000 +
        ((interface->interface_id + interface->tcp->next_sequence) & 0xFF));
    u64 connection = tcp_active_open(
        interface->tcp, interface->ipv4_address, local_port,
        destination, port);
    if (!connection) return -1;
    struct tcp_transmit transmit;
    if (tcp_prepare_transmit(interface->tcp, connection,
                             interface->now, &transmit) != 1 ||
        transmit_tcp(interface, &transmit) < 0) {
        tcp_close(interface->tcp, connection);
        return -1;
    }
    interface->tcp_probe_connection = connection;
    interface->tcp_probe_sent = 0;
    interface->tcp_probe_complete = 0;
    return 0;
}

int net_interface_tcp_probe_poll(struct kernel_object *object,
                                 struct driver_domain *owner) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !owner_valid(interface, owner) ||
        !interface->tcp_probe_connection)
        return -1;
    u32 state;
    if (tcp_connection_state(interface->tcp,
                             interface->tcp_probe_connection, &state))
        return -1;
    if (state != TCP_STATE_ESTABLISHED) return 0;
    if (!interface->tcp_probe_sent) {
        const u8 payload[8] = {'M', 'I', 'C', 'H', 'T', 'C', 'P', 0};
        struct tcp_transmit transmit;
        if (tcp_queue_send(interface->tcp,
                           interface->tcp_probe_connection,
                           payload, sizeof(payload)) ||
            tcp_prepare_transmit(interface->tcp,
                                 interface->tcp_probe_connection,
                                 interface->now, &transmit) != 1 ||
            transmit_tcp(interface, &transmit) < 0)
            return -1;
        interface->tcp_probe_sent = 1;
        return 0;
    }
    u8 received[16];
    u32 length = 0;
    if (tcp_receive_data(interface->tcp,
                         interface->tcp_probe_connection,
                         received, sizeof(received), &length) || !length)
        return 0;
    const u8 expected[8] = {'M', 'I', 'C', 'H', 'T', 'C', 'P', 0};
    if (length != sizeof(expected)) return -1;
    for (u32 index = 0; index < sizeof(expected); index++)
        if (received[index] != expected[index]) return -1;
    interface->tcp_probe_complete = 1;
    return 1;
}

int net_interface_tcpv6_probe_start(struct kernel_object *object,
                                    struct driver_domain *owner, u16 port) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !owner_valid(interface, owner) || !interface->tcp ||
        interface->ipv6_state != NET_INTERFACE_IPV6_SLAAC ||
        interface->tcpv6_probe_connection || !port)
        return -1;
    u8 destination[16];
    for (u32 byte = 0; byte < 8; byte++)
        destination[byte] = interface->ipv6_global[byte];
    for (u32 byte = 8; byte < 16; byte++) destination[byte] = 0;
    destination[15] = 4;
    u64 connection = tcp_active_open_ipv6(
        interface->tcp, interface->ipv6_global, 56000,
        destination, port);
    if (!connection) return -1;
    struct tcp_transmit transmit;
    if (tcp_prepare_transmit(interface->tcp, connection,
                             interface->now, &transmit) != 1 ||
        transmit_tcp(interface, &transmit) < 0) {
        tcp_close(interface->tcp, connection);
        return -1;
    }
    interface->tcpv6_probe_connection = connection;
    interface->tcpv6_probe_sent = 0;
    interface->tcpv6_probe_complete = 0;
    return 0;
}

int net_interface_tcpv6_probe_poll(struct kernel_object *object,
                                   struct driver_domain *owner) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !owner_valid(interface, owner) ||
        !interface->tcp || !interface->tcpv6_probe_connection)
        return -1;
    u32 state;
    if (tcp_connection_state(interface->tcp,
                             interface->tcpv6_probe_connection, &state))
        return -1;
    if (state != TCP_STATE_ESTABLISHED) return 0;
    if (!interface->tcpv6_probe_sent) {
        const u8 payload[8] = {'M', 'I', 'C', 'H', '6', 'T', 'C', 'P'};
        struct tcp_transmit transmit;
        if (tcp_queue_send(interface->tcp,
                           interface->tcpv6_probe_connection,
                           payload, sizeof(payload)) ||
            tcp_prepare_transmit(interface->tcp,
                                 interface->tcpv6_probe_connection,
                                 interface->now, &transmit) != 1 ||
            transmit_tcp(interface, &transmit) < 0)
            return -1;
        interface->tcpv6_probe_sent = 1;
        return 0;
    }
    u8 received[16];
    u32 length = 0;
    if (tcp_receive_data(interface->tcp,
                         interface->tcpv6_probe_connection,
                         received, sizeof(received), &length) || !length)
        return 0;
    const u8 expected[8] = {'M', 'I', 'C', 'H', '6', 'T', 'C', 'P'};
    if (length != sizeof(expected)) return -1;
    for (u32 index = 0; index < sizeof(expected); index++)
        if (received[index] != expected[index]) return -1;
    interface->tcpv6_probe_complete = 1;
    return 1;
}

int net_interface_tcp_connect(struct kernel_object *object,
                              u32 destination, u16 port,
                              struct tcp_context **tcp, u64 *connection) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !interface->ipv4_ready || !interface->tcp ||
        !destination || !port || !tcp || !connection)
        return -1;
    u16 local_port = (u16)(49152 +
        ((interface->interface_id + interface->tcp->next_sequence) & 0x3FFF));
    u64 id = tcp_active_open(interface->tcp, interface->ipv4_address,
                             local_port, destination, port);
    if (!id) return -1;
    struct tcp_transmit transmit;
    if (tcp_prepare_transmit(interface->tcp, id, interface->now,
                             &transmit) != 1 ||
        transmit_tcp(interface, &transmit) < 0) {
        tcp_close(interface->tcp, id);
        return -1;
    }
    *tcp = interface->tcp;
    *connection = id;
    return 0;
}

int net_interface_tcp_listen(struct kernel_object *object, u16 port,
                             struct tcp_context **tcp, u64 *listener) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !interface->ipv4_ready || !interface->tcp ||
        !port || !tcp || !listener)
        return -1;
    u64 id = tcp_listen(interface->tcp, interface->ipv4_address, port);
    if (!id) return -1;
    *tcp = interface->tcp;
    *listener = id;
    return 0;
}

int net_interface_tcp_listen_ipv6(struct kernel_object *object, u16 port,
                                  struct tcp_context **tcp, u64 *listener) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || interface->ipv6_state != NET_INTERFACE_IPV6_SLAAC ||
        !interface->tcp || !port || !tcp || !listener)
        return -1;
    u64 id = tcp_listen_ipv6(interface->tcp, interface->ipv6_global, port);
    if (!id) return -1;
    *tcp = interface->tcp;
    *listener = id;
    return 0;
}

int net_interface_tcp_accept(struct kernel_object *object, u64 listener,
                             u64 *connection) {
    struct net_interface *interface = net_interface_get(object);
    return interface && interface->tcp ?
        tcp_accept(interface->tcp, listener, connection) : -1;
}

int net_interface_tcp_send(struct kernel_object *object, u64 connection,
                           const void *data, u32 length) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !interface->tcp ||
        tcp_queue_send(interface->tcp, connection, data, length))
        return -1;
    return flush_tcp(interface, connection);
}

int net_interface_tcp_send_pages(struct kernel_object *object,
                                 u64 connection,
                                 struct kernel_object *pages, u32 offset,
                                 u32 length) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !interface->tcp ||
        tcp_queue_send_pages(interface->tcp, connection, pages, offset,
                             length))
        return -1;
    return flush_tcp(interface, connection);
}

int net_interface_tcp_receive(struct kernel_object *object, u64 connection,
                              void *data, u32 capacity, u32 *received) {
    struct net_interface *interface = net_interface_get(object);
    return interface && interface->tcp ?
        tcp_receive_data(interface->tcp, connection,
                         data, capacity, received) : -1;
}

int net_interface_tcp_receive_pages(struct kernel_object *object,
                                    u64 connection,
                                    struct kernel_object *pages,
                                    u32 offset, u32 length) {
    struct net_interface *interface = net_interface_get(object);
    return interface && interface->tcp ?
        tcp_queue_receive_pages(interface->tcp, connection, pages, offset,
                                length) : -1;
}

int net_interface_tcp_shutdown(struct kernel_object *object, u64 connection) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !interface->tcp) return -1;
    struct tcp_transmit transmit;
    int result = tcp_shutdown(interface->tcp, connection,
                              interface->now, &transmit);
    if (result < 0) return -1;
    if (result == 0) return transmit_tcp(interface, &transmit);
    return flush_tcp(interface, connection);
}

int net_interface_tcp_state(struct kernel_object *object, u64 connection,
                            u32 *state) {
    struct net_interface *interface = net_interface_get(object);
    return interface && interface->tcp ?
        tcp_connection_state(interface->tcp, connection, state) : -1;
}

int net_interface_tcp_take_error(struct kernel_object *object,
                                 u64 connection, i32 *error) {
    struct net_interface *interface = net_interface_get(object);
    return interface && interface->tcp ?
        tcp_take_error(interface->tcp, connection, error) : -1;
}

int net_interface_tcp_close(struct kernel_object *object, u64 connection) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !interface->tcp) return -1;
    struct tcp_transmit transmit;
    int result = tcp_detach(interface->tcp, connection,
                            interface->now, &transmit);
    if (result < 0) return -1;
    if (result > 0 && transmit_tcp(interface, &transmit)) return -1;
    return flush_tcp(interface, connection);
}

int net_interface_ipv6_start(struct kernel_object *object,
                             struct driver_domain *owner) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || interface->state != NET_INTERFACE_UP ||
        !owner_valid(interface, owner) ||
        interface->ipv6_state != NET_INTERFACE_IPV6_DISABLED ||
        ipv6_link_local_from_mac(interface->mac,
                                 interface->ipv6_link_local) ||
        ethernet_port_set_filter(&interface->ethernet, 0, 1) ||
        ipv6_init(&interface->ipv6, &interface->ethernet,
                  interface->ipv6_link_local, 1) ||
        icmpv6_init(&interface->icmpv6, &interface->ipv6,
                    interface->ipv6_link_local, interface->mac, 1,
                    transmit_icmpv6, interface) ||
        icmpv6_set_pmtu_callback(&interface->icmpv6, interface_pmtu6,
                                 interface) ||
        udpv6_init(&interface->udpv6, &interface->ipv6,
                   transmit_udpv6, interface) ||
        ipv6_register_protocol(&interface->ipv6, 6,
                               interface_tcpv6_handler, interface))
        return -1;
    u8 unspecified[IPV6_ADDRESS_SIZE];
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++) unspecified[index] = 0;
    u8 destination[IPV6_ADDRESS_SIZE];
    u8 message[32];
    u32 length;
    if (icmpv6_build_neighbor_solicitation(
            unspecified, interface->ipv6_link_local, interface->mac,
            message, sizeof(message), destination, &length) ||
        transmit_ipv6_packet(interface, unspecified, destination,
                             58, message, length))
        return -1;
    interface->ipv6_state = NET_INTERFACE_IPV6_TENTATIVE;
    event_signal(interface->event);
    return 0;
}

static int send_router_solicitation(struct net_interface *interface) {
    u8 all_routers[IPV6_ADDRESS_SIZE];
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++) all_routers[index] = 0;
    all_routers[0] = 0xFF;
    all_routers[1] = 0x02;
    all_routers[15] = 2;
    u8 message[16];
    u32 length;
    if (icmpv6_build_router_solicitation(
            interface->ipv6_link_local, interface->mac, all_routers,
            message, sizeof(message), &length))
        return -1;
    return transmit_ipv6_packet(
        interface, interface->ipv6_link_local,
        all_routers, 58, message, length);
}

int net_interface_ipv6_complete_dad(struct kernel_object *object,
                                    struct driver_domain *owner) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !owner_valid(interface, owner) ||
        interface->ipv6_state != NET_INTERFACE_IPV6_TENTATIVE ||
        interface->icmpv6.duplicate)
        return -1;
    interface->icmpv6.tentative = 0;
    interface->ipv6_state = NET_INTERFACE_IPV6_LINK_LOCAL;
    if (send_router_solicitation(interface)) return -1;
    interface->ipv6_rs_retries = 1;
    interface->ipv6_rs_deadline = interface->now + 400;
    event_signal(interface->event);
    return 0;
}

static int apply_slaac(struct net_interface *interface) {
    if (!interface || interface->ipv6_state != NET_INTERFACE_IPV6_LINK_LOCAL ||
        !interface->icmpv6.prefix_autonomous ||
        interface->icmpv6.prefix_length != 64 ||
        !interface->icmpv6.prefix_valid_lifetime)
        return 0;
    for (u32 index = 0; index < 8; index++)
        interface->ipv6_global[index] = interface->icmpv6.prefix[index];
    for (u32 index = 8; index < IPV6_ADDRESS_SIZE; index++)
        interface->ipv6_global[index] = interface->ipv6_link_local[index];
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
        interface->ipv6_router[index] = interface->icmpv6.router[index];
    if (ipv6_add_local_address(&interface->ipv6,
                               interface->ipv6_global) ||
        socket_register_udpv6(&interface->udpv6,
                              interface->interface_id,
                              interface->generation))
        return -1;
    interface->ipv6_state = NET_INTERFACE_IPV6_SLAAC;
    interface->ipv6_deprecated =
        interface->icmpv6.prefix_preferred_lifetime == 0;
    interface->ipv6_rs_retries = 0;
    interface->ipv6_rs_deadline = 0;
    event_signal(interface->event);
    return 1;
}

int net_interface_ipv6_send_echo(struct kernel_object *object,
                                 struct driver_domain *owner) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !owner_valid(interface, owner) ||
        interface->ipv6_state != NET_INTERFACE_IPV6_SLAAC ||
        ipv6_address_unspecified(interface->ipv6_router))
        return -1;
    u8 destination[IPV6_ADDRESS_SIZE];
    for (u32 index = 0; index < 8; index++)
        destination[index] = interface->icmpv6.prefix[index];
    for (u32 index = 8; index < IPV6_ADDRESS_SIZE; index++) destination[index] = 0;
    destination[15] = 2;
    u8 message[32];
    for (u32 index = 0; index < sizeof(message); index++) message[index] = 0;
    message[0] = ICMPV6_ECHO_REQUEST;
    message[4] = 0x4D;
    message[5] = 0x36;
    message[7] = 1;
    for (u32 index = 8; index < sizeof(message); index++) message[index] = (u8)index;
    u16 checksum = icmpv6_checksum(
        interface->ipv6_global, destination, message, sizeof(message));
    message[2] = (u8)(checksum >> 8);
    message[3] = (u8)checksum;
    return transmit_ipv6_packet(
        interface, interface->ipv6_global,
        destination, 58, message, sizeof(message));
}

u64 net_interface_ipv6_echo_replies(struct kernel_object *object,
                                    struct driver_domain *owner) {
    struct net_interface *interface = net_interface_get(object);
    return interface && owner_valid(interface, owner) ?
        interface->icmpv6.stats.echo_replies : 0;
}

int net_interface_udpv6_start_probe(struct kernel_object *object,
                                    struct driver_domain *owner) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !owner_valid(interface, owner) ||
        interface->ipv6_state != NET_INTERFACE_IPV6_SLAAC)
        return -1;
    u8 destination[IPV6_ADDRESS_SIZE];
    for (u32 index = 0; index < 8; index++)
        destination[index] = interface->icmpv6.prefix[index];
    for (u32 index = 8; index < IPV6_ADDRESS_SIZE; index++) destination[index] = 0;
    destination[15] = 3;
    u8 query[29];
    for (u32 index = 0; index < sizeof(query); index++) query[index] = 0;
    query[0] = 0x4D;
    query[1] = 0x36;
    query[2] = 0x01;
    query[5] = 1;
    query[12] = 7;
    query[13] = 'e';
    query[14] = 'x';
    query[15] = 'a';
    query[16] = 'm';
    query[17] = 'p';
    query[18] = 'l';
    query[19] = 'e';
    query[20] = 3;
    query[21] = 'c';
    query[22] = 'o';
    query[23] = 'm';
    query[25] = 0;
    query[26] = 1;
    query[27] = 0;
    query[28] = 1;
    if (interface->udpv6_probe_binding)
        return udpv6_send(&interface->udpv6,
                          interface->udpv6_probe_binding, destination, 53,
                          query, sizeof(query));
    u64 binding = udpv6_bind(
        &interface->udpv6, interface->ipv6_global, 0);
    if (!binding) return -1;
    if (udpv6_send(&interface->udpv6, binding, destination, 53,
                   query, sizeof(query))) {
        udpv6_unbind(&interface->udpv6, binding);
        return -1;
    }
    interface->udpv6_probe_binding = binding;
    interface->udpv6_probe_complete = 0;
    return 0;
}

int net_interface_udpv6_poll_probe(struct kernel_object *object,
                                   struct driver_domain *owner) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !owner_valid(interface, owner) ||
        !interface->udpv6_probe_binding)
        return -1;
    struct udpv6_datagram datagram;
    if (udpv6_receive(&interface->udpv6,
                      interface->udpv6_probe_binding, &datagram)) {
        if (!interface->icmpv6.stats.destination_unreachable) return 0;
        if (udpv6_unbind(&interface->udpv6,
                         interface->udpv6_probe_binding))
            return -1;
        interface->udpv6_probe_binding = 0;
        return 2;
    }
    int valid = datagram.source_port == 53 && datagram.length >= 12 &&
        datagram.payload[0] == 0x4D && datagram.payload[1] == 0x36 &&
        (datagram.payload[2] & 0x80);
    if (udpv6_unbind(&interface->udpv6,
                     interface->udpv6_probe_binding))
        return -1;
    interface->udpv6_probe_binding = 0;
    interface->udpv6_probe_complete = valid != 0;
    return valid ? 1 : -1;
}

int net_interface_ipv6_info(struct kernel_object *object,
                            struct driver_domain *owner,
                            u32 *state, u32 *duplicate, u32 *deprecated,
                            u8 link_local[IPV6_ADDRESS_SIZE],
                            u8 global[IPV6_ADDRESS_SIZE],
                            u8 router[IPV6_ADDRESS_SIZE]) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !owner_valid(interface, owner) || !state ||
        !duplicate || !deprecated || !link_local || !global || !router)
        return -1;
    if (apply_slaac(interface) < 0) return -1;
    *state = interface->ipv6_state;
    *duplicate = interface->icmpv6.duplicate;
    *deprecated = interface->ipv6_deprecated;
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++) {
        link_local[index] = interface->ipv6_link_local[index];
        global[index] = interface->ipv6_global[index];
        router[index] = interface->ipv6_router[index];
    }
    return 0;
}

u64 net_interface_acquire_tx(struct kernel_object *object) {
    struct net_interface *interface = net_interface_get(object);
    return interface && interface->state == NET_INTERFACE_UP ?
        packet_pool_acquire(interface->pool, NET_BUFFER_TX) : 0;
}

void *net_interface_packet_data(struct kernel_object *object, u64 buffer_id) {
    struct net_interface *interface = net_interface_get(object);
    return interface ? packet_pool_data(interface->pool, buffer_id,
                                        NET_BUFFER_TX) : 0;
}

int net_interface_send_ipv4(struct kernel_object *object, u64 buffer_id,
                            u32 offset, u32 length, u32 destination,
                            u32 next_hop, u32 now) {
    struct net_interface *interface = net_interface_get(object);
    u8 *data = interface ? (u8 *)packet_pool_data(
        interface->pool, buffer_id, NET_BUFFER_TX) : 0;
    if (!interface || interface->state != NET_INTERFACE_UP ||
        !interface->arp_ready || !data || !destination || !next_hop ||
        offset < 14 || offset >= NET_PACKET_DATA_MAX || !length ||
        length > NET_PACKET_DATA_MAX - offset)
        return -1;
    if (length > interface->mtu) {
        interface->stats.mtu_drops++;
        interface->stats.tx_drops++;
        return -1;
    }
    interface->now = now;
    arp_tick(&interface->arp, now);
    u8 hardware[6];
    if (!arp_lookup(&interface->arp, next_hop, hardware, now)) {
        u32 frame_offset = offset - 14;
        for (u32 index = 0; index < 6; index++) {
            data[frame_offset + index] = hardware[index];
            data[frame_offset + 6 + index] = interface->mac[index];
        }
        data[frame_offset + 12] = 0x08;
        data[frame_offset + 13] = 0x00;
        return publish_frame(interface, buffer_id, frame_offset, length + 14);
    }
    if (interface->pending_count >= NET_INTERFACE_PENDING_MAX) {
        interface->stats.tx_drops++;
        return -1;
    }
    u32 slot = NET_INTERFACE_PENDING_MAX;
    for (u32 index = 0; index < NET_INTERFACE_PENDING_MAX; index++)
        if (!interface->pending[index].active) {
            slot = index;
            break;
        }
    if (slot == NET_INTERFACE_PENDING_MAX) return -1;
    struct net_interface_pending *pending = &interface->pending[slot];
    pending->buffer_id = buffer_id;
    pending->destination = destination;
    pending->next_hop = next_hop;
    pending->expires = now + 300;
    pending->offset = (u16)offset;
    pending->length = (u16)length;
    pending->active = 1;
    interface->pending_count++;
    int resolved = arp_resolve(&interface->arp, next_hop, now);
    if (resolved < 0) {
        pending->active = 0;
        interface->pending_count--;
        return -1;
    }
    interface->stats.arp_queued++;
    return 1;
}

int net_interface_route_send(struct route_table *routes, u64 buffer_id,
                             u32 offset, u32 length,
                             u32 destination, u32 now) {
    const struct route_entry *route = route_lookup(routes, destination);
    if (!route || !route->interface_generation) return -1;
    struct kernel_object *interface = net_interface_lookup(
        route->interface_id, route->interface_generation);
    if (!interface) return -1;
    u32 next_hop = route->gateway ? route->gateway : destination;
    return net_interface_send_ipv4(interface, buffer_id, offset, length,
                                   destination, next_hop, now);
}

static int net_interface_receive_frame_ctx(struct net_interface *interface,
                                           const void *frame, u32 length,
                                           u32 now) {
    interface->now = now;
    // Throttle the ARP cache sweep: it is periodic housekeeping, so run it at
    // most once per NET_INTERFACE_ARP_RX_TICK_MS instead of on every frame.
    // The timer path (net_interface_tick) still runs it unconditionally.
    if (interface->arp_ready &&
        (i32)(now - interface->last_arp_tick) >= NET_INTERFACE_ARP_RX_TICK_MS) {
        arp_tick(&interface->arp, now);
        interface->last_arp_tick = now;
    }
    int result = ethernet_receive(&interface->ethernet, frame, length);
    if (!result) {
        interface->stats.rx_packets++;
        interface->stats.rx_bytes += length;
    } else {
        interface->stats.rx_drops++;
    }
    return result;
}

int net_interface_receive_frame(struct kernel_object *object,
                                const void *frame, u32 length, u32 now) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || interface->state != NET_INTERFACE_UP || !frame)
        return -1;
    return net_interface_receive_frame_ctx(interface, frame, length, now);
}

u64 net_interface_driver_acquire_rx(struct kernel_object *object,
                                    struct driver_domain *owner) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || interface->state != NET_INTERFACE_UP ||
        !owner_valid(interface, owner))
        return 0;
    return packet_pool_acquire(interface->pool, NET_BUFFER_DRIVER_RX);
}

int net_interface_driver_receive(struct kernel_object *object,
                                 struct driver_domain *owner, u64 buffer_id,
                                 u32 offset, u32 length, u32 now) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || interface->state != NET_INTERFACE_UP ||
        !owner_valid(interface, owner) || offset >= NET_PACKET_DATA_MAX ||
        !length || length > NET_PACKET_DATA_MAX - offset)
        return -1;
    void *buffer = packet_pool_data(interface->pool, buffer_id,
                                    NET_BUFFER_DRIVER_RX);
    if (!buffer) return -1;
    int result = net_interface_receive_frame(
        object, (u8 *)buffer + offset, length, now);
    if (packet_pool_release(interface->pool, buffer_id,
                            NET_BUFFER_DRIVER_RX))
        return -1;
    return result;
}

int net_interface_driver_release_rx(struct kernel_object *object,
                                    struct driver_domain *owner,
                                    u64 buffer_id) {
    struct net_interface *interface = net_interface_get(object);
    return interface && owner_valid(interface, owner) ?
        packet_pool_release(interface->pool, buffer_id,
                            NET_BUFFER_DRIVER_RX) : -1;
}

int net_interface_driver_dequeue_tx(struct kernel_object *object,
                                    struct driver_domain *owner,
                                    struct net_packet_descriptor *descriptor) {
    struct net_interface *interface = net_interface_get(object);
    struct ring_resource *ring = interface ?
        ring_resource_get(interface->tx_ring) : 0;
    if (!interface || interface->state != NET_INTERFACE_UP ||
        !owner_valid(interface, owner) || !descriptor || !ring ||
        ring->producer == ring->consumer)
        return -1;
    struct net_packet_descriptor *source =
        ring_resource_descriptor(interface->tx_ring, ring->consumer);
    if (!source) return -1;
    if (source->reserved0 || !source->length ||
        source->offset >= NET_PACKET_DATA_MAX ||
        source->length > NET_PACKET_DATA_MAX - source->offset) {
        ring_resource_consume(interface->tx_ring, 1);
        packet_pool_release(interface->pool, source->buffer_id,
                            NET_BUFFER_TX_QUEUED);
        interface->stats.tx_drops++;
        return -1;
    }
    if (packet_pool_transition(interface->pool, source->buffer_id,
                               NET_BUFFER_TX_QUEUED,
                               NET_BUFFER_DRIVER_TX))
        return -1;
    *descriptor = *source;
    if (ring_resource_consume(interface->tx_ring, 1)) {
        packet_pool_transition(interface->pool, source->buffer_id,
                               NET_BUFFER_DRIVER_TX,
                               NET_BUFFER_TX_QUEUED);
        return -1;
    }
    return 0;
}

int net_interface_driver_complete_tx(struct kernel_object *object,
                                     struct driver_domain *owner,
                                     u64 buffer_id) {
    struct net_interface *interface = net_interface_get(object);
    return interface && owner_valid(interface, owner) ?
        packet_pool_release(interface->pool, buffer_id,
                            NET_BUFFER_DRIVER_TX) : -1;
}

u32 net_interface_driver_acquire_rx_batch(struct kernel_object *object,
        struct driver_domain *owner, u32 count, u64 *buffer_ids) {
    if (!buffer_ids || !count || count > NET_INTERFACE_BATCH_MAX)
        return 0;
    u32 acquired = 0;
    while (acquired < count) {
        u64 buffer_id = net_interface_driver_acquire_rx(object, owner);
        if (!buffer_id) break;
        buffer_ids[acquired++] = buffer_id;
    }
    return acquired;
}

u32 net_interface_driver_receive_batch(struct kernel_object *object,
        struct driver_domain *owner,
        const struct net_interface_buffer_request *requests, u32 count,
        u32 now) {
    if (!requests || !count || count > NET_INTERFACE_BATCH_MAX)
        return 0;
    struct net_interface *interface = net_interface_get(object);
    if (!interface || interface->state != NET_INTERFACE_UP ||
        !owner_valid(interface, owner))
        return 0;
    u32 processed = 0;
    while (processed < count) {
        const struct net_interface_buffer_request *request =
            &requests[processed];
        if (request->offset >= NET_PACKET_DATA_MAX || !request->length ||
            request->length > NET_PACKET_DATA_MAX - request->offset)
            break;
        void *buffer = packet_pool_data(interface->pool, request->buffer_id,
                                        NET_BUFFER_DRIVER_RX);
        if (!buffer)
            break;
        if (net_interface_receive_frame_ctx(
                interface, (const u8 *)buffer + request->offset,
                request->length, now) ||
            packet_pool_release(interface->pool, request->buffer_id,
                                NET_BUFFER_DRIVER_RX))
            break;
        processed++;
    }
    return processed;
}

u32 net_interface_driver_dequeue_tx_batch(struct kernel_object *object,
        struct driver_domain *owner,
        struct net_packet_descriptor *descriptors, u32 max_count) {
    if (!descriptors || !max_count || max_count > NET_INTERFACE_BATCH_MAX)
        return 0;
    u32 dequeued = 0;
    while (dequeued < max_count) {
        if (net_interface_driver_dequeue_tx(object, owner,
                &descriptors[dequeued]))
            break;
        dequeued++;
    }
    return dequeued;
}

u32 net_interface_driver_complete_tx_batch(struct kernel_object *object,
        struct driver_domain *owner, const u64 *buffer_ids, u32 count) {
    if (!buffer_ids || !count || count > NET_INTERFACE_BATCH_MAX)
        return 0;
    u32 completed = 0;
    while (completed < count) {
        if (net_interface_driver_complete_tx(object, owner,
                buffer_ids[completed]))
            break;
        completed++;
    }
    return completed;
}

struct kernel_object *net_interface_pool(struct kernel_object *object,
                                         struct driver_domain *owner) {
    struct net_interface *interface = net_interface_get(object);
    return interface && owner_valid(interface, owner) ? interface->pool : 0;
}

void net_interface_tick(struct kernel_object *object, u32 now) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || interface->state == NET_INTERFACE_REVOKED) return;
    interface->now = now;
    pmtu_tick(&interface->pmtu, now);
    if (interface->arp_ready) arp_tick(&interface->arp, now);
    if (interface->ipv4_ready) {
        icmp_tick(&interface->icmp, now);
        for (u32 budget = 0; budget < 8; budget++) {
            struct tcp_transmit transmit;
            int ready = tcp_tick(interface->tcp, now, &transmit);
            if (ready <= 0) break;
            if (ready == 2) {
                socket_tcp_notify(interface->tcp, transmit.connection_id);
                continue;
            }
            if (transmit_tcp(interface, &transmit) < 0) break;
            // A PMTU blackhole RTO may have rewound a MSS tail into
            // send_buffer; push it while cwnd still allows.
            if (transmit.retransmission &&
                flush_tcp(interface, transmit.connection_id) < 0)
                break;
        }
    }
    if (interface->ipv6_state != NET_INTERFACE_IPV6_DISABLED)
        icmpv6_tick(&interface->icmpv6, now);
    if (interface->ipv6_state == NET_INTERFACE_IPV6_SLAAC &&
        !interface->icmpv6.prefix_preferred_lifetime)
        interface->ipv6_deprecated = 1;
    if (interface->ipv6_state == NET_INTERFACE_IPV6_SLAAC &&
        !interface->icmpv6.prefix_valid_lifetime) {
        socket_unregister_udpv6(&interface->udpv6);
        if (interface->udpv6_probe_binding) {
            udpv6_unbind(&interface->udpv6,
                         interface->udpv6_probe_binding);
            interface->udpv6_probe_binding = 0;
        }
        ipv6_remove_local_address(&interface->ipv6,
                                  interface->ipv6_global);
        for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
            interface->ipv6_global[index] = 0;
        interface->ipv6_state = NET_INTERFACE_IPV6_LINK_LOCAL;
        interface->ipv6_deprecated = 0;
        interface->ipv6_rs_retries = 0;
        interface->ipv6_rs_deadline = now;
    }
    if ((interface->ipv6_state == NET_INTERFACE_IPV6_LINK_LOCAL ||
         (interface->ipv6_state == NET_INTERFACE_IPV6_SLAAC &&
          !interface->icmpv6.router_lifetime)) &&
        interface->ipv6_rs_retries < 3 &&
        (i32)(now - interface->ipv6_rs_deadline) >= 0) {
        if (!send_router_solicitation(interface)) {
            interface->ipv6_rs_retries++;
            interface->ipv6_rs_deadline = now + 400;
        }
    }
    for (u32 index = 0; index < NET_INTERFACE_PENDING_MAX; index++) {
        struct net_interface_pending *pending = &interface->pending[index];
        if (!pending->active) continue;
        if ((i32)(now - pending->expires) >= 0) {
            packet_pool_release(interface->pool, pending->buffer_id,
                                NET_BUFFER_TX);
            pending->active = 0;
            if (interface->pending_count) interface->pending_count--;
            interface->stats.arp_timeouts++;
            interface->stats.tx_drops++;
            continue;
        }
        u8 hardware[6];
        if (interface->arp_ready &&
            !arp_lookup(&interface->arp, pending->next_hop, hardware, now))
            flush_pending(interface, pending->next_hop, hardware);
    }
}

int net_interface_revoke(struct kernel_object *object) {
    return revoke_interface(net_interface_get(object));
}

int net_interface_remove(struct kernel_object *object) {
    struct net_interface *interface = net_interface_get(object);
    if (!interface || !interface->registered) return -1;
    int result = interface->state == NET_INTERFACE_REVOKED ? 0
                 : revoke_interface(interface);
    for (u32 index = 0; index < NET_INTERFACE_MAX; index++)
        if (registry[index] == object) {
            registry[index] = 0;
            interface->registered = 0;
            interface->state = NET_INTERFACE_REMOVED;
            object_release(object);
            return result;
        }
    return -1;
}

struct kernel_object *net_interface_lookup(u32 interface_id, u32 generation) {
    if (!interface_id || !generation) return 0;
    for (u32 index = 0; index < NET_INTERFACE_MAX; index++) {
        struct kernel_object *object = registry[index];
        struct net_interface *interface = net_interface_get(object);
        if (interface && interface->interface_id == interface_id &&
            interface->generation == generation &&
            interface->state != NET_INTERFACE_REVOKED &&
            interface->state != NET_INTERFACE_REMOVED)
            return object;
    }
    return 0;
}

struct kernel_object *net_interface_wait_event(struct kernel_object *object) {
    struct net_interface *interface = net_interface_get(object);
    return interface ? interface->event : 0;
}

void net_interface_task_died(int pid) {
    if (!pid) return;
    for (u32 index = 0; index < NET_INTERFACE_MAX; index++) {
        struct kernel_object *object = registry[index];
        struct net_interface *interface = net_interface_get(object);
        if (!interface || !interface->owner || interface->owner->pid != pid)
            continue;
        if (interface->state != NET_INTERFACE_REVOKED)
            revoke_interface(interface);
        handle_revoke_object(object);
        net_interface_remove(object);
    }
}

u32 net_interface_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < NET_INTERFACE_MAX; index++)
        if (interfaces[index].active) count++;
    return count;
}
