#include "checksum.h"
#include "udp.h"
#include "event.h"
#include "net_buffer.h"

static u16 read_be16(const u8 *bytes) {
    return (u16)((u16)bytes[0] << 8) | bytes[1];
}

static void write_be16(u8 *bytes, u16 value) {
    bytes[0] = (u8)(value >> 8);
    bytes[1] = (u8)value;
}

u16 udp_checksum(u32 source, u32 destination,
                 const void *datagram, u32 length) {
    if (!datagram || length < UDP_HEADER_SIZE || length > 0xFFFFu)
        return 0xFFFF;
    u8 pseudo[12];
    pseudo[0] = (u8)(source >> 24);
    pseudo[1] = (u8)(source >> 16);
    pseudo[2] = (u8)(source >> 8);
    pseudo[3] = (u8)source;
    pseudo[4] = (u8)(destination >> 24);
    pseudo[5] = (u8)(destination >> 16);
    pseudo[6] = (u8)(destination >> 8);
    pseudo[7] = (u8)destination;
    pseudo[8] = 0;
    pseudo[9] = 17;
    pseudo[10] = (u8)(length >> 8);
    pseudo[11] = (u8)length;
    u32 sum = net_checksum_sum(0, pseudo, sizeof(pseudo));
    sum = net_checksum_sum(sum, (const u8 *)datagram, length);
    return net_checksum_finish(sum);
}

static struct udp_binding *binding_for(struct udp_context *udp, u64 id,
                                       u32 *slot_out) {
    u32 low = (u32)id;
    u32 generation = (u32)(id >> 32);
    if (!udp || !low || low > UDP_BINDING_MAX || !generation) return 0;
    u32 slot = low - 1;
    struct udp_binding *binding = &udp->bindings[slot];
    if (!binding->active || binding->generation != generation) return 0;
    if (slot_out) *slot_out = slot;
    return binding;
}

int udp_init(struct udp_context *udp, struct ipv4_context *ipv4) {
    if (!udp || !ipv4) return -1;
    udp->ipv4 = ipv4;
    udp->unreachable = 0;
    udp->unreachable_context = 0;
    u32 ephemeral_count = UDP_EPHEMERAL_LAST - UDP_EPHEMERAL_FIRST + 1;
    udp->next_ephemeral = UDP_EPHEMERAL_FIRST +
        ((ipv4->local_address ^ 0x4D494348u) % ephemeral_count);
    for (u32 index = 0; index < UDP_BINDING_MAX; index++) {
        if (udp->bindings[index].active && udp->bindings[index].event)
            object_release(udp->bindings[index].event);
        udp->bindings[index].generation++;
        if (!udp->bindings[index].generation)
            udp->bindings[index].generation = 1;
        udp->bindings[index].local_address = 0;
        udp->bindings[index].port = 0;
        udp->bindings[index].event = 0;
        udp->bindings[index].head = 0;
        udp->bindings[index].count = 0;
        udp->bindings[index].active = 0;
    }
    u8 *stats = (u8 *)&udp->stats;
    for (usize_t index = 0; index < sizeof(udp->stats); index++) stats[index] = 0;
    return ipv4_register_protocol(ipv4, 17, udp_ipv4_handler, udp);
}

static int port_conflicts(struct udp_context *udp, u32 local_address,
                          u16 port) {
    for (u32 index = 0; index < UDP_BINDING_MAX; index++) {
        struct udp_binding *binding = &udp->bindings[index];
        if (!binding->active || binding->port != port) continue;
        if (!binding->local_address || !local_address ||
            binding->local_address == local_address)
            return 1;
    }
    return 0;
}

int udp_set_unreachable_callback(struct udp_context *udp,
                                 udp_unreachable_fn function, void *context) {
    if (!udp || !function) return -1;
    udp->unreachable = function;
    udp->unreachable_context = context;
    return 0;
}

u64 udp_bind(struct udp_context *udp, u32 local_address, u16 port) {
    if (!udp || (local_address && local_address != udp->ipv4->local_address))
        return 0;
    if (!port) {
        u32 count = UDP_EPHEMERAL_LAST - UDP_EPHEMERAL_FIRST + 1;
        u32 candidate = udp->next_ephemeral;
        u32 searched = 0;
        while (searched < count &&
               port_conflicts(udp, local_address, (u16)candidate)) {
            candidate = candidate == UDP_EPHEMERAL_LAST
                ? UDP_EPHEMERAL_FIRST : candidate + 1;
            searched++;
        }
        if (searched == count) return 0;
        port = (u16)candidate;
        udp->next_ephemeral = candidate == UDP_EPHEMERAL_LAST
            ? UDP_EPHEMERAL_FIRST : candidate + 1;
    } else if (port_conflicts(udp, local_address, port)) {
        return 0;
    }
    for (u32 index = 0; index < UDP_BINDING_MAX; index++) {
        struct udp_binding *binding = &udp->bindings[index];
        if (binding->active) continue;
        struct kernel_object *event = event_create(EVENT_AUTO_RESET, 0);
        if (!event) return 0;
        binding->local_address = local_address;
        binding->port = port;
        binding->event = event;
        binding->head = 0;
        binding->count = 0;
        binding->active = 1;
        return ((u64)binding->generation << 32) | (index + 1);
    }
    return 0;
}

int udp_unbind(struct udp_context *udp, u64 binding_id) {
    struct udp_binding *binding = binding_for(udp, binding_id, 0);
    if (!binding) return -1;
    for (u32 index = 0; index < binding->count; index++) {
        u32 slot = (binding->head + index) % UDP_QUEUE_MAX;
        struct udp_datagram *datagram = &binding->queue[slot];
        if (datagram->zero_copy && datagram->packet_pool)
            packet_pool_release(datagram->packet_pool, datagram->buffer_id,
                                NET_BUFFER_STACK);
    }
    binding->local_address = 0;
    binding->port = 0;
    if (binding->event) object_release(binding->event);
    binding->event = 0;
    binding->head = 0;
    binding->count = 0;
    binding->active = 0;
    binding->generation++;
    if (!binding->generation) binding->generation = 1;
    return 0;
}

int udp_send(struct udp_context *udp, u64 binding_id,
             u32 destination_address, u16 destination_port,
             const void *payload, u32 payload_length) {
    struct udp_binding *binding = binding_for(udp, binding_id, 0);
    if (!binding || !destination_address || !destination_port ||
        (!payload && payload_length) || payload_length > UDP_PAYLOAD_MAX)
        return -1;
    u32 length = UDP_HEADER_SIZE + payload_length;
    u8 datagram[UDP_HEADER_SIZE + UDP_PAYLOAD_MAX];
    write_be16(datagram, binding->port);
    write_be16(datagram + 2, destination_port);
    write_be16(datagram + 4, (u16)length);
    datagram[6] = 0;
    datagram[7] = 0;
    net_copy(datagram + UDP_HEADER_SIZE, payload, payload_length);
    u32 source = binding->local_address ? binding->local_address
                                        : udp->ipv4->local_address;
    u16 checksum = udp_checksum(source, destination_address, datagram, length);
    if (!checksum) checksum = 0xFFFF;
    write_be16(datagram + 6, checksum);
    if (ipv4_send(udp->ipv4, destination_address, 17,
                  datagram, length) < 0)
        return -1;
    udp->stats.transmitted++;
    udp->stats.bytes_transmitted += payload_length;
    return 0;
}

static struct udp_binding *find_destination(struct udp_context *udp,
                                            u32 address, u16 port) {
    struct udp_binding *wildcard = 0;
    for (u32 index = 0; index < UDP_BINDING_MAX; index++) {
        struct udp_binding *binding = &udp->bindings[index];
        if (!binding->active || binding->port != port) continue;
        if (binding->local_address == address) return binding;
        if (!binding->local_address) wildcard = binding;
    }
    return wildcard;
}

int udp_ipv4_handler(struct ipv4_context *ipv4,
                     const struct ipv4_packet_view *packet,
                     void *context) {
    struct udp_context *udp = (struct udp_context *)context;
    if (!ipv4 || !packet || !udp || udp->ipv4 != ipv4 ||
        packet->protocol != 17 || packet->payload_length < UDP_HEADER_SIZE) {
        if (udp) udp->stats.drops_length++;
        return -1;
    }
    const u8 *bytes = packet->payload;
    u16 source_port = read_be16(bytes);
    u16 destination_port = read_be16(bytes + 2);
    u32 length = read_be16(bytes + 4);
    if (!source_port || !destination_port || length < UDP_HEADER_SIZE ||
        length > packet->payload_length) {
        udp->stats.drops_length++;
        return -1;
    }
    u16 checksum = read_be16(bytes + 6);
    if (checksum && udp_checksum(packet->source, packet->destination,
                                 bytes, length)) {
        udp->stats.drops_checksum++;
        return -1;
    }
    struct udp_binding *binding = find_destination(
        udp, packet->destination, destination_port);
    if (!binding) {
        udp->stats.drops_port++;
        if (udp->unreachable)
            udp->unreachable(packet, udp->unreachable_context);
        return -1;
    }
    u32 payload_length = length - UDP_HEADER_SIZE;
    if (payload_length > UDP_PAYLOAD_MAX ||
        (!packet->packet_pool && payload_length > UDP_INLINE_PAYLOAD_MAX)) {
        udp->stats.drops_oversized++;
        return -1;
    }
    if (binding->count >= UDP_QUEUE_MAX) {
        udp->stats.drops_queue++;
        return -1;
    }
    u32 slot = (binding->head + binding->count) % UDP_QUEUE_MAX;
    struct udp_datagram *datagram = &binding->queue[slot];
    datagram->source_address = packet->source;
    datagram->destination_address = packet->destination;
    datagram->source_port = source_port;
    datagram->destination_port = destination_port;
    datagram->length = (u16)payload_length;
    datagram->reserved = 0;
    datagram->packet_pool = packet->packet_pool;
    datagram->buffer_id = packet->buffer_id;
    datagram->payload_offset = packet->packet_offset + packet->header_length +
                               UDP_HEADER_SIZE;
    datagram->zero_copy = packet->packet_pool != 0;
    if (!datagram->zero_copy)
        net_copy(datagram->payload, bytes + UDP_HEADER_SIZE, payload_length);
    binding->count++;
    event_signal(binding->event);
    udp->stats.received++;
    udp->stats.bytes_received += payload_length;
    return datagram->zero_copy ? 1 : 0;
}

int udp_receive(struct udp_context *udp, u64 binding_id,
                struct udp_datagram *datagram) {
    struct udp_binding *binding = binding_for(udp, binding_id, 0);
    if (!binding || !datagram || !binding->count) return -1;
    struct udp_datagram *queued = &binding->queue[binding->head];
    datagram->source_address = queued->source_address;
    datagram->destination_address = queued->destination_address;
    datagram->source_port = queued->source_port;
    datagram->destination_port = queued->destination_port;
    datagram->length = queued->length;
    datagram->reserved = 0;
    datagram->packet_pool = 0;
    datagram->buffer_id = 0;
    datagram->payload_offset = 0;
    datagram->zero_copy = 0;
    if (queued->zero_copy) {
        u8 *data = (u8 *)packet_pool_data(
            queued->packet_pool, queued->buffer_id, NET_BUFFER_STACK);
        if (!data || queued->payload_offset > NET_PACKET_DATA_MAX ||
            queued->length > NET_PACKET_DATA_MAX - queued->payload_offset)
            return -1;
        net_copy(datagram->payload, data + queued->payload_offset,
                 queued->length);
        if (packet_pool_release(queued->packet_pool, queued->buffer_id,
                                NET_BUFFER_STACK))
            return -1;
    } else {
        net_copy(datagram->payload, queued->payload, queued->length);
    }
    queued->packet_pool = 0;
    queued->buffer_id = 0;
    queued->zero_copy = 0;
    binding->head = (binding->head + 1) % UDP_QUEUE_MAX;
    binding->count--;
    event_reset(binding->event);
    if (binding->count) event_signal(binding->event);
    return 0;
}

struct kernel_object *udp_binding_event(struct udp_context *udp,
                                        u64 binding_id) {
    struct udp_binding *binding = binding_for(udp, binding_id, 0);
    return binding ? binding->event : 0;
}

int udp_binding_local(struct udp_context *udp, u64 binding_id,
                      u32 *address, u16 *port) {
    struct udp_binding *binding = binding_for(udp, binding_id, 0);
    if (!binding || !address || !port) return -1;
    *address = binding->local_address ? binding->local_address
                                      : udp->ipv4->local_address;
    *port = binding->port;
    return 0;
}

u32 udp_binding_count(const struct udp_context *udp) {
    if (!udp) return 0;
    u32 count = 0;
    for (u32 index = 0; index < UDP_BINDING_MAX; index++)
        if (udp->bindings[index].active) count++;
    return count;
}
