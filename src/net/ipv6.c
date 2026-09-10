#include "ipv6.h"

static u16 read_be16(const u8 *bytes) {
    return (u16)((u16)bytes[0] << 8) | bytes[1];
}

static void write_be16(u8 *bytes, u16 value) {
    bytes[0] = (u8)(value >> 8);
    bytes[1] = (u8)value;
}

int ipv6_address_equal(const u8 left[IPV6_ADDRESS_SIZE],
                       const u8 right[IPV6_ADDRESS_SIZE]) {
    if (!left || !right) return 0;
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
        if (left[index] != right[index]) return 0;
    return 1;
}

int ipv6_address_unspecified(const u8 address[IPV6_ADDRESS_SIZE]) {
    if (!address) return 0;
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
        if (address[index]) return 0;
    return 1;
}

int ipv6_address_multicast(const u8 address[IPV6_ADDRESS_SIZE]) {
    return address && address[0] == 0xFF;
}

int ipv6_address_link_local(const u8 address[IPV6_ADDRESS_SIZE]) {
    return address && address[0] == 0xFE && (address[1] & 0xC0) == 0x80;
}

int ipv6_link_local_from_mac(const u8 mac[6],
                             u8 address[IPV6_ADDRESS_SIZE]) {
    if (!mac || !address || (mac[0] & 1)) return -1;
    u32 nonzero = 0;
    for (u32 index = 0; index < 6; index++) nonzero |= mac[index];
    if (!nonzero) return -1;
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++) address[index] = 0;
    address[0] = 0xFE;
    address[1] = 0x80;
    address[8] = mac[0] ^ 2;
    address[9] = mac[1];
    address[10] = mac[2];
    address[11] = 0xFF;
    address[12] = 0xFE;
    address[13] = mac[3];
    address[14] = mac[4];
    address[15] = mac[5];
    return 0;
}

int ipv6_solicited_node_address(const u8 unicast[IPV6_ADDRESS_SIZE],
                                u8 multicast[IPV6_ADDRESS_SIZE]) {
    if (!unicast || !multicast || ipv6_address_multicast(unicast) ||
        ipv6_address_unspecified(unicast))
        return -1;
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++) multicast[index] = 0;
    multicast[0] = 0xFF;
    multicast[1] = 0x02;
    multicast[11] = 0x01;
    multicast[12] = 0xFF;
    multicast[13] = unicast[13];
    multicast[14] = unicast[14];
    multicast[15] = unicast[15];
    return 0;
}

static int unicast_address(const u8 address[IPV6_ADDRESS_SIZE]) {
    return address && !ipv6_address_unspecified(address) &&
           !ipv6_address_multicast(address);
}

static int extension_header(u8 next_header) {
    return next_header == 0 || next_header == 43 || next_header == 44 ||
           next_header == 51 || next_header == 60;
}

int ipv6_parse(const void *data, u32 length,
               struct ipv6_packet_view *packet) {
    if (!data || !packet || length < IPV6_HEADER_SIZE) return -1;
    const u8 *bytes = data;
    if ((bytes[0] >> 4) != 6) return -2;
    u32 payload_length = read_be16(bytes + 4);
    if (payload_length > length - IPV6_HEADER_SIZE) return -1;
    if (!bytes[7]) return -3;
    if (ipv6_address_multicast(bytes + 8)) return -4;
    packet->packet = bytes;
    packet->packet_length = IPV6_HEADER_SIZE + payload_length;
    packet->traffic_class = (u8)((bytes[0] << 4) | (bytes[1] >> 4));
    packet->flow_label = ((u32)(bytes[1] & 0x0F) << 16) |
                         ((u32)bytes[2] << 8) | bytes[3];
    packet->hop_limit = bytes[7];
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++) {
        packet->source[index] = bytes[8 + index];
        packet->destination[index] = bytes[24 + index];
    }
    u8 next = bytes[6];
    u32 offset = IPV6_HEADER_SIZE;
    u32 remaining = payload_length;
    u32 extensions = 0;
    while (extension_header(next)) {
        if (extensions >= IPV6_EXTENSION_MAX || remaining < 8) return -5;
        if (next == 44) return -6;
        if (next == 43) return -5;
        u32 header_length = next == 51 ?
            ((u32)bytes[offset + 1] + 2) * 4 :
            ((u32)bytes[offset + 1] + 1) * 8;
        if (header_length < 8 || header_length > remaining) return -5;
        next = bytes[offset];
        offset += header_length;
        remaining -= header_length;
        extensions++;
    }
    packet->payload = bytes + offset;
    packet->payload_offset = offset;
    packet->payload_length = remaining;
    packet->next_header = next;
    packet->extension_count = (u8)extensions;
    return 0;
}

static int ipv6_address_loopback(const u8 address[IPV6_ADDRESS_SIZE]) {
    if (!address || address[15] != 1) return 0;
    for (u32 index = 0; index < 15; index++)
        if (address[index]) return 0;
    return 1;
}

static int dispatch(struct ipv6_context *ipv6,
                    const struct ipv6_packet_view *packet) {
    if (ipv6_address_unspecified(packet->source) &&
        packet->next_header != 58) {
        ipv6->stats.drops_source++;
        return -1;
    }
    if (!ipv6_address_unspecified(packet->source) &&
        ipv6_has_local_address(ipv6, packet->source) &&
        !ipv6_has_local_address(ipv6, packet->destination) &&
        !ipv6_address_multicast(packet->destination)) {
        ipv6->stats.drops_spoof++;
        return -1;
    }
    if (!ipv6_address_multicast(packet->destination) &&
        ipv6_address_equal(packet->source, packet->destination) &&
        !ipv6_has_local_address(ipv6, packet->destination)) {
        ipv6->stats.drops_land++;
        return -1;
    }
    if (ipv6_address_loopback(packet->source) &&
        !ipv6_has_local_address(ipv6, packet->source)) {
        ipv6->stats.drops_source++;
        return -1;
    }
    int multicast = ipv6_address_multicast(packet->destination);
    int local = 0;
    if (!multicast) {
        if (ipv6_address_equal(packet->destination, ipv6->local_address))
            local = 1;
        for (u32 index = 1; index < ipv6->address_count; index++)
            if (ipv6_address_equal(
                    packet->destination,
                    ipv6->additional_addresses[index - 1]))
                local = 1;
    }
    if (!multicast && !local) {
        ipv6->stats.drops_destination++;
        return -1;
    }
    if (multicast && !ipv6->accept_multicast) {
        ipv6->stats.drops_destination++;
        return -1;
    }
    ipv6->stats.packets++;
    ipv6->stats.bytes += packet->packet_length;
    if (multicast) ipv6->stats.multicast++;
    else ipv6->stats.unicast++;
    for (u32 index = 0; index < ipv6->handler_count; index++) {
        struct ipv6_protocol_handler *handler = &ipv6->handlers[index];
        if (handler->protocol != packet->next_header) continue;
        int result = handler->function(ipv6, packet, handler->context);
        if (result < 0) ipv6->stats.drops_protocol++;
        return result;
    }
    ipv6->stats.drops_protocol++;
    return -1;
}

int ipv6_receive(struct ipv6_context *ipv6, const void *data, u32 length) {
    if (!ipv6) return -1;
    struct ipv6_packet_view packet;
    int result = ipv6_parse(data, length, &packet);
    if (result) {
        if (result == -1) ipv6->stats.drops_length++;
        else if (result == -2) ipv6->stats.drops_version++;
        else if (result == -3) ipv6->stats.drops_hop_limit++;
        else if (result == -4) ipv6->stats.drops_source++;
        else if (result == -6) ipv6->stats.drops_fragment++;
        else ipv6->stats.drops_extension++;
        return -1;
    }
    return dispatch(ipv6, &packet);
}

int ipv6_ethernet_handler(struct ethernet_port *port,
                          const struct ethernet_frame_view *frame,
                          void *context) {
    struct ipv6_context *ipv6 = context;
    if (!port || !frame || !ipv6 || ipv6->port != port ||
        frame->ether_type != 0x86DD)
        return -1;
    return ipv6_receive(ipv6, frame->payload, frame->payload_length);
}

int ipv6_init(struct ipv6_context *ipv6, struct ethernet_port *port,
              const u8 local_address[IPV6_ADDRESS_SIZE],
              int accept_multicast) {
    if (!ipv6 || !port || !unicast_address(local_address)) return -1;
    ipv6->port = port;
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
        ipv6->local_address[index] = local_address[index];
    for (u32 slot = 0; slot < IPV6_LOCAL_ADDRESS_MAX - 1; slot++)
        for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
            ipv6->additional_addresses[slot][index] = 0;
    ipv6->address_count = 1;
    ipv6->accept_multicast = accept_multicast != 0;
    ipv6->handler_count = 0;
    for (u32 index = 0; index < IPV6_PROTOCOL_HANDLER_MAX; index++) {
        ipv6->handlers[index].protocol = 0;
        ipv6->handlers[index].function = 0;
        ipv6->handlers[index].context = 0;
    }
    u8 *stats = (u8 *)&ipv6->stats;
    for (usize_t index = 0; index < sizeof(ipv6->stats); index++) stats[index] = 0;
    return ethernet_register_handler(port, 0x86DD,
                                     ipv6_ethernet_handler, ipv6);
}

int ipv6_has_local_address(const struct ipv6_context *ipv6,
                           const u8 address[IPV6_ADDRESS_SIZE]) {
    if (!ipv6 || !address) return 0;
    if (ipv6_address_equal(ipv6->local_address, address)) return 1;
    for (u32 index = 1; index < ipv6->address_count; index++)
        if (ipv6_address_equal(ipv6->additional_addresses[index - 1],
                               address))
            return 1;
    return 0;
}

int ipv6_remove_local_address(struct ipv6_context *ipv6,
                              const u8 address[IPV6_ADDRESS_SIZE]) {
    if (!ipv6 || !address || ipv6->address_count <= 1 ||
        ipv6_address_equal(ipv6->local_address, address))
        return -1;
    for (u32 index = 1; index < ipv6->address_count; index++) {
        if (!ipv6_address_equal(ipv6->additional_addresses[index - 1],
                                address))
            continue;
        for (u32 move = index; move + 1 < ipv6->address_count; move++)
            for (u32 byte = 0; byte < IPV6_ADDRESS_SIZE; byte++)
                ipv6->additional_addresses[move - 1][byte] =
                    ipv6->additional_addresses[move][byte];
        for (u32 byte = 0; byte < IPV6_ADDRESS_SIZE; byte++)
            ipv6->additional_addresses[ipv6->address_count - 2][byte] = 0;
        ipv6->address_count--;
        return 0;
    }
    return -1;
}

int ipv6_add_local_address(struct ipv6_context *ipv6,
                           const u8 address[IPV6_ADDRESS_SIZE]) {
    if (!ipv6 || !unicast_address(address) ||
        ipv6->address_count >= IPV6_LOCAL_ADDRESS_MAX)
        return -1;
    if (ipv6_address_equal(ipv6->local_address, address)) return -1;
    for (u32 index = 1; index < ipv6->address_count; index++)
        if (ipv6_address_equal(ipv6->additional_addresses[index - 1],
                               address))
            return -1;
    u8 *destination =
        ipv6->additional_addresses[ipv6->address_count - 1];
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
        destination[index] = address[index];
    ipv6->address_count++;
    return 0;
}

int ipv6_register_protocol(struct ipv6_context *ipv6, u8 protocol,
                           ipv6_protocol_fn function, void *context) {
    if (!ipv6 || !protocol || !function ||
        ipv6->handler_count >= IPV6_PROTOCOL_HANDLER_MAX)
        return -1;
    for (u32 index = 0; index < ipv6->handler_count; index++)
        if (ipv6->handlers[index].protocol == protocol) return -1;
    struct ipv6_protocol_handler *handler =
        &ipv6->handlers[ipv6->handler_count++];
    handler->protocol = protocol;
    handler->function = function;
    handler->context = context;
    return 0;
}

int ipv6_build_header(void *buffer, u32 buffer_length, u32 payload_length,
                      const u8 source[IPV6_ADDRESS_SIZE],
                      const u8 destination[IPV6_ADDRESS_SIZE],
                      u8 next_header, u8 hop_limit,
                      u8 traffic_class, u32 flow_label) {
    if (!buffer || buffer_length < IPV6_HEADER_SIZE ||
        payload_length > 0xFFFFu ||
        payload_length > buffer_length - IPV6_HEADER_SIZE ||
        (!unicast_address(source) && !ipv6_address_unspecified(source)) ||
        !destination ||
        ipv6_address_unspecified(destination) || !next_header ||
        !hop_limit || flow_label > 0xFFFFFu)
        return -1;
    u8 *bytes = buffer;
    bytes[0] = 0x60 | (traffic_class >> 4);
    bytes[1] = (u8)(traffic_class << 4) | (u8)(flow_label >> 16);
    bytes[2] = (u8)(flow_label >> 8);
    bytes[3] = (u8)flow_label;
    write_be16(bytes + 4, (u16)payload_length);
    bytes[6] = next_header;
    bytes[7] = hop_limit;
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++) {
        bytes[8 + index] = source[index];
        bytes[24 + index] = destination[index];
    }
    return 0;
}
