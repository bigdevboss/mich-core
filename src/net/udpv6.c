#include "checksum.h"
#include "udpv6.h"
#include "event.h"

static u16 read_be16(const u8 *bytes) {
    return (u16)((u16)bytes[0] << 8) | bytes[1];
}

static void write_be16(u8 *bytes, u16 value) {
    bytes[0] = (u8)(value >> 8);
    bytes[1] = (u8)value;
}


u16 udpv6_checksum(const u8 source[IPV6_ADDRESS_SIZE],
                   const u8 destination[IPV6_ADDRESS_SIZE],
                   const void *datagram, u32 length) {
    if (!source || !destination || !datagram ||
        length < UDPV6_HEADER_SIZE || length > 0xFFFFu)
        return 0xFFFF;
    u32 sum = net_checksum_sum(0, source, IPV6_ADDRESS_SIZE);
    sum = net_checksum_sum(sum, destination, IPV6_ADDRESS_SIZE);
    u8 tail[8];
    tail[0] = (u8)(length >> 24);
    tail[1] = (u8)(length >> 16);
    tail[2] = (u8)(length >> 8);
    tail[3] = (u8)length;
    tail[4] = 0;
    tail[5] = 0;
    tail[6] = 0;
    tail[7] = 17;
    sum = net_checksum_sum(sum, tail, sizeof(tail));
    sum = net_checksum_sum(sum, datagram, length);
    return net_checksum_finish(sum);
}

static struct udpv6_binding *binding_for(struct udpv6_context *udp,
                                         u64 id) {
    u32 low = (u32)id;
    u32 generation = (u32)(id >> 32);
    if (!udp || !low || low > UDPV6_BINDING_MAX || !generation) return 0;
    struct udpv6_binding *binding = &udp->bindings[low - 1];
    return binding->active && binding->generation == generation ? binding : 0;
}

static int port_conflicts(struct udpv6_context *udp,
                          const u8 address[IPV6_ADDRESS_SIZE], u16 port) {
    for (u32 index = 0; index < UDPV6_BINDING_MAX; index++) {
        struct udpv6_binding *binding = &udp->bindings[index];
        if (binding->active && binding->port == port &&
            ipv6_address_equal(binding->local_address, address))
            return 1;
    }
    return 0;
}

int udpv6_init(struct udpv6_context *udp, struct ipv6_context *ipv6,
               udpv6_transmit_fn transmit, void *transmit_context) {
    if (!udp || !ipv6 || !transmit) return -1;
    udp->ipv6 = ipv6;
    udp->transmit = transmit;
    udp->transmit_context = transmit_context;
    udp->next_ephemeral = UDPV6_EPHEMERAL_FIRST;
    for (u32 index = 0; index < UDPV6_BINDING_MAX; index++) {
        if (udp->bindings[index].active && udp->bindings[index].event)
            object_release(udp->bindings[index].event);
        udp->bindings[index].generation++;
        if (!udp->bindings[index].generation)
            udp->bindings[index].generation = 1;
        for (u32 byte = 0; byte < IPV6_ADDRESS_SIZE; byte++)
            udp->bindings[index].local_address[byte] = 0;
        udp->bindings[index].port = 0;
        udp->bindings[index].event = 0;
        udp->bindings[index].head = 0;
        udp->bindings[index].count = 0;
        udp->bindings[index].active = 0;
    }
    u8 *stats = (u8 *)&udp->stats;
    for (usize_t index = 0; index < sizeof(udp->stats); index++) stats[index] = 0;
    return ipv6_register_protocol(ipv6, 17, udpv6_ipv6_handler, udp);
}

u64 udpv6_bind(struct udpv6_context *udp,
               const u8 local_address[IPV6_ADDRESS_SIZE], u16 port) {
    if (!udp || !local_address ||
        !ipv6_has_local_address(udp->ipv6, local_address))
        return 0;
    if (!port) {
        u32 count = UDPV6_EPHEMERAL_LAST - UDPV6_EPHEMERAL_FIRST + 1;
        u32 candidate = udp->next_ephemeral;
        u32 searched = 0;
        while (searched < count &&
               port_conflicts(udp, local_address, (u16)candidate)) {
            candidate = candidate == UDPV6_EPHEMERAL_LAST ?
                UDPV6_EPHEMERAL_FIRST : candidate + 1;
            searched++;
        }
        if (searched == count) return 0;
        port = (u16)candidate;
        udp->next_ephemeral = candidate == UDPV6_EPHEMERAL_LAST ?
            UDPV6_EPHEMERAL_FIRST : candidate + 1;
    } else if (port_conflicts(udp, local_address, port)) {
        return 0;
    }
    for (u32 index = 0; index < UDPV6_BINDING_MAX; index++) {
        struct udpv6_binding *binding = &udp->bindings[index];
        if (binding->active) continue;
        struct kernel_object *event = event_create(EVENT_AUTO_RESET, 0);
        if (!event) return 0;
        for (u32 byte = 0; byte < IPV6_ADDRESS_SIZE; byte++)
            binding->local_address[byte] = local_address[byte];
        binding->port = port;
        binding->event = event;
        binding->head = 0;
        binding->count = 0;
        binding->active = 1;
        return ((u64)binding->generation << 32) | (index + 1);
    }
    return 0;
}

int udpv6_unbind(struct udpv6_context *udp, u64 binding_id) {
    struct udpv6_binding *binding = binding_for(udp, binding_id);
    if (!binding) return -1;
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

int udpv6_send(struct udpv6_context *udp, u64 binding_id,
               const u8 destination[IPV6_ADDRESS_SIZE], u16 destination_port,
               const void *payload, u32 payload_length) {
    struct udpv6_binding *binding = binding_for(udp, binding_id);
    if (!binding || !destination || ipv6_address_unspecified(destination) ||
        !destination_port || (!payload && payload_length) ||
        payload_length > UDPV6_PAYLOAD_MAX)
        return -1;
    u32 length = UDPV6_HEADER_SIZE + payload_length;
    u8 datagram[UDPV6_HEADER_SIZE + UDPV6_PAYLOAD_MAX];
    write_be16(datagram, binding->port);
    write_be16(datagram + 2, destination_port);
    write_be16(datagram + 4, (u16)length);
    datagram[6] = 0;
    datagram[7] = 0;
    net_copy(datagram + UDPV6_HEADER_SIZE, payload, payload_length);
    u16 checksum = udpv6_checksum(binding->local_address, destination,
                                  datagram, length);
    if (!checksum) checksum = 0xFFFF;
    write_be16(datagram + 6, checksum);
    if (udp->transmit(binding->local_address, destination,
                      datagram, length, udp->transmit_context) < 0)
        return -1;
    udp->stats.transmitted++;
    udp->stats.bytes_transmitted += payload_length;
    return 0;
}

static struct udpv6_binding *find_destination(
    struct udpv6_context *udp, const u8 address[IPV6_ADDRESS_SIZE], u16 port) {
    for (u32 index = 0; index < UDPV6_BINDING_MAX; index++) {
        struct udpv6_binding *binding = &udp->bindings[index];
        if (binding->active && binding->port == port &&
            ipv6_address_equal(binding->local_address, address))
            return binding;
    }
    return 0;
}

int udpv6_ipv6_handler(struct ipv6_context *ipv6,
                       const struct ipv6_packet_view *packet,
                       void *context) {
    struct udpv6_context *udp = context;
    if (!ipv6 || !packet || !udp || udp->ipv6 != ipv6 ||
        packet->next_header != 17 || packet->payload_length < UDPV6_HEADER_SIZE) {
        if (udp) udp->stats.drops_length++;
        return -1;
    }
    const u8 *bytes = packet->payload;
    u16 source_port = read_be16(bytes);
    u16 destination_port = read_be16(bytes + 2);
    u32 length = read_be16(bytes + 4);
    if (!source_port || !destination_port || length < UDPV6_HEADER_SIZE ||
        length > packet->payload_length) {
        udp->stats.drops_length++;
        return -1;
    }
    if (!read_be16(bytes + 6) ||
        udpv6_checksum(packet->source, packet->destination, bytes, length)) {
        udp->stats.drops_checksum++;
        return -1;
    }
    struct udpv6_binding *binding = find_destination(
        udp, packet->destination, destination_port);
    if (!binding) {
        udp->stats.drops_port++;
        return -1;
    }
    u32 payload_length = length - UDPV6_HEADER_SIZE;
    if (payload_length > UDPV6_PAYLOAD_MAX) {
        udp->stats.drops_oversized++;
        return -1;
    }
    if (binding->count >= UDPV6_QUEUE_MAX) {
        udp->stats.drops_queue++;
        return -1;
    }
    u32 slot = (binding->head + binding->count) % UDPV6_QUEUE_MAX;
    struct udpv6_datagram *datagram = &binding->queue[slot];
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++) {
        datagram->source[index] = packet->source[index];
        datagram->destination[index] = packet->destination[index];
    }
    datagram->source_port = source_port;
    datagram->destination_port = destination_port;
    datagram->length = (u16)payload_length;
    net_copy(datagram->payload, bytes + UDPV6_HEADER_SIZE, payload_length);
    binding->count++;
    event_signal(binding->event);
    udp->stats.received++;
    udp->stats.bytes_received += payload_length;
    return 0;
}

int udpv6_receive(struct udpv6_context *udp, u64 binding_id,
                  struct udpv6_datagram *datagram) {
    struct udpv6_binding *binding = binding_for(udp, binding_id);
    if (!binding || !datagram || !binding->count) return -1;
    *datagram = binding->queue[binding->head];
    binding->head = (binding->head + 1) % UDPV6_QUEUE_MAX;
    binding->count--;
    event_reset(binding->event);
    if (binding->count) event_signal(binding->event);
    return 0;
}

struct kernel_object *udpv6_binding_event(struct udpv6_context *udp,
                                          u64 binding_id) {
    struct udpv6_binding *binding = binding_for(udp, binding_id);
    return binding ? binding->event : 0;
}
