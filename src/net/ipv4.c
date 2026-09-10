#include "checksum.h"
#include "ipv4.h"
#include "net_buffer.h"

static u16 read_be16(const u8 *bytes) {
    return (u16)((u16)bytes[0] << 8) | bytes[1];
}

static u32 read_be32(const u8 *bytes) {
    return ((u32)bytes[0] << 24) | ((u32)bytes[1] << 16) |
           ((u32)bytes[2] << 8) | bytes[3];
}

static void write_be16(u8 *bytes, u16 value) {
    bytes[0] = (u8)(value >> 8);
    bytes[1] = (u8)value;
}

static void write_be32(u8 *bytes, u32 value) {
    bytes[0] = (u8)(value >> 24);
    bytes[1] = (u8)(value >> 16);
    bytes[2] = (u8)(value >> 8);
    bytes[3] = (u8)value;
}

static int unicast_address(u32 address) {
    u8 first = (u8)(address >> 24);
    return address && address != 0xFFFFFFFFu && first != 0 && first < 224;
}

static int netmask_valid(u32 netmask) {
    if (!netmask) return 0;
    u32 inverse = ~netmask;
    return !(inverse & (inverse + 1));
}

u16 ipv4_checksum(const void *data, u32 length) {
    if (!data || !length) return 0xFFFF;
    return net_checksum_finish(net_checksum_sum(0, (const u8 *)data, length));
}

int ipv4_parse(const void *data, u32 length,
               struct ipv4_packet_view *packet) {
    if (!data || !packet || length < IPV4_HEADER_MIN) return -1;
    const u8 *bytes = (const u8 *)data;
    u8 version = bytes[0] >> 4;
    u32 header_length = (u32)(bytes[0] & 0x0F) * 4;
    if (version != 4 || header_length < IPV4_HEADER_MIN) return -2;
    if (header_length > length) return -1;
    u32 total_length = read_be16(bytes + 2);
    if (total_length < header_length || total_length > length) return -1;
    if (ipv4_checksum(bytes, header_length)) return -3;
    if (!bytes[8]) return -4;
    u32 source = read_be32(bytes + 12);
    if (!unicast_address(source)) return -5;
    u16 fragment = read_be16(bytes + 6);
    if ((fragment & 0x8000u) || (fragment & 0x3FFFu)) return -6;
    packet->packet = bytes;
    packet->header = bytes;
    packet->payload = bytes + header_length;
    packet->packet_pool = 0;
    packet->buffer_id = 0;
    packet->packet_offset = 0;
    packet->packet_length = total_length;
    packet->header_length = header_length;
    packet->payload_length = total_length - header_length;
    packet->source = source;
    packet->destination = read_be32(bytes + 16);
    packet->identification = read_be16(bytes + 4);
    packet->fragment = fragment;
    packet->ttl = bytes[8];
    packet->protocol = bytes[9];
    packet->dscp_ecn = bytes[1];
    return 0;
}

int ipv4_init(struct ipv4_context *ipv4, struct ethernet_port *port,
              u32 local_address, u32 netmask, int accept_broadcast) {
    if (!ipv4 || !port || !unicast_address(local_address) ||
        !netmask_valid(netmask))
        return -1;
    ipv4->port = port;
    ipv4->local_address = local_address;
    ipv4->netmask = netmask;
    ipv4->subnet_broadcast = (local_address & netmask) | ~netmask;
    ipv4->accept_broadcast = accept_broadcast != 0;
    ipv4->transmit = 0;
    ipv4->transmit_context = 0;
    ipv4->handler_count = 0;
    for (u32 index = 0; index < IPV4_PROTOCOL_HANDLER_MAX; index++) {
        ipv4->handlers[index].protocol = 0;
        ipv4->handlers[index].function = 0;
        ipv4->handlers[index].context = 0;
    }
    u8 *stats = (u8 *)&ipv4->stats;
    for (usize_t index = 0; index < sizeof(ipv4->stats); index++) stats[index] = 0;
    return ethernet_register_handler(port, 0x0800,
                                     ipv4_ethernet_handler, ipv4);
}

int ipv4_set_transmit(struct ipv4_context *ipv4,
                      ipv4_transmit_fn transmit, void *context) {
    if (!ipv4 || !transmit ||
        (ipv4->transmit && (ipv4->transmit != transmit ||
                            ipv4->transmit_context != context)))
        return -1;
    ipv4->transmit = transmit;
    ipv4->transmit_context = context;
    return 0;
}

int ipv4_send(struct ipv4_context *ipv4, u32 destination, u8 protocol,
              const void *payload, u32 payload_length) {
    if (!ipv4 || !ipv4->transmit || !destination || !protocol ||
        (!payload && payload_length) || payload_length > 0xFFFFu - IPV4_HEADER_MIN)
        return -1;
    return ipv4->transmit(ipv4, destination, protocol, payload,
                          payload_length, ipv4->transmit_context);
}

int ipv4_register_protocol(struct ipv4_context *ipv4, u8 protocol,
                           ipv4_protocol_fn function, void *context) {
    if (!ipv4 || !protocol || !function ||
        ipv4->handler_count >= IPV4_PROTOCOL_HANDLER_MAX)
        return -1;
    for (u32 index = 0; index < ipv4->handler_count; index++)
        if (ipv4->handlers[index].protocol == protocol) return -1;
    struct ipv4_protocol_handler *handler =
        &ipv4->handlers[ipv4->handler_count++];
    handler->protocol = protocol;
    handler->function = function;
    handler->context = context;
    return 0;
}

static int dispatch_packet(struct ipv4_context *ipv4,
                           struct ipv4_packet_view *packet) {
    int source_loopback = (packet->source >> 24) == 127;
    int local_loopback = (ipv4->local_address >> 24) == 127;
    if (source_loopback != local_loopback) {
        ipv4->stats.drops_source++;
        return -1;
    }
    if (!local_loopback) {
        if (packet->source == packet->destination) {
            ipv4->stats.drops_land++;
            return -1;
        }
        if (packet->source == ipv4->local_address) {
            ipv4->stats.drops_spoof++;
            return -1;
        }
    }
    int broadcast = packet->destination == 0xFFFFFFFFu ||
                    packet->destination == ipv4->subnet_broadcast;
    if (packet->destination != ipv4->local_address &&
        !(broadcast && ipv4->accept_broadcast)) {
        ipv4->stats.drops_destination++;
        return -1;
    }
    ipv4->stats.packets++;
    ipv4->stats.bytes += packet->packet_length;
    if (broadcast) ipv4->stats.broadcast++;
    else ipv4->stats.unicast++;
    for (u32 index = 0; index < ipv4->handler_count; index++) {
        struct ipv4_protocol_handler *handler = &ipv4->handlers[index];
        if (handler->protocol != packet->protocol) continue;
        int result = handler->function(ipv4, packet, handler->context);
        if (result < 0) {
            ipv4->stats.drops_protocol++;
            return -1;
        }
        return result;
    }
    ipv4->stats.drops_protocol++;
    return -1;
}

static int parse_and_count(struct ipv4_context *ipv4, const void *data,
                           u32 length, struct ipv4_packet_view *packet) {
    int parsed = ipv4_parse(data, length, packet);
    if (!parsed) return 0;
    if (parsed == -1) ipv4->stats.drops_length++;
    else if (parsed == -2) ipv4->stats.drops_version++;
    else if (parsed == -3) ipv4->stats.drops_checksum++;
    else if (parsed == -4) ipv4->stats.drops_ttl++;
    else if (parsed == -5) ipv4->stats.drops_source++;
    else ipv4->stats.drops_fragment++;
    return -1;
}

int ipv4_receive(struct ipv4_context *ipv4, const void *data, u32 length) {
    if (!ipv4) return -1;
    struct ipv4_packet_view packet;
    if (parse_and_count(ipv4, data, length, &packet)) return -1;
    return dispatch_packet(ipv4, &packet);
}

int ipv4_receive_buffer(struct ipv4_context *ipv4,
                        struct kernel_object *packet_pool, u64 buffer_id,
                        u32 offset, u32 length) {
    if (!ipv4 || !packet_pool || offset >= NET_PACKET_DATA_MAX || !length ||
        length > NET_PACKET_DATA_MAX - offset)
        return -1;
    u8 *data = (u8 *)packet_pool_data(packet_pool, buffer_id, NET_BUFFER_STACK);
    if (!data) return -1;
    struct ipv4_packet_view packet;
    if (parse_and_count(ipv4, data + offset, length, &packet)) return -1;
    packet.packet_pool = packet_pool;
    packet.buffer_id = buffer_id;
    packet.packet_offset = offset;
    return dispatch_packet(ipv4, &packet);
}

int ipv4_ethernet_handler(struct ethernet_port *port,
                          const struct ethernet_frame_view *frame,
                          void *context) {
    struct ipv4_context *ipv4 = (struct ipv4_context *)context;
    if (!port || !frame || !ipv4 || ipv4->port != port ||
        frame->ether_type != 0x0800)
        return -1;
    return ipv4_receive(ipv4, frame->payload, frame->payload_length);
}

int ipv4_build_header(void *buffer, u32 buffer_length, u32 payload_length,
                      u32 source, u32 destination, u8 protocol, u8 ttl,
                      u16 identification, int dont_fragment) {
    if (!buffer || buffer_length < IPV4_HEADER_MIN ||
        payload_length > 0xFFFFu - IPV4_HEADER_MIN ||
        !unicast_address(source) || !destination || !protocol || !ttl)
        return -1;
    u8 *header = (u8 *)buffer;
    for (u32 index = 0; index < IPV4_HEADER_MIN; index++) header[index] = 0;
    header[0] = 0x45;
    write_be16(header + 2, (u16)(IPV4_HEADER_MIN + payload_length));
    write_be16(header + 4, identification);
    write_be16(header + 6, dont_fragment ? 0x4000 : 0);
    header[8] = ttl;
    header[9] = protocol;
    write_be32(header + 12, source);
    write_be32(header + 16, destination);
    write_be16(header + 10, ipv4_checksum(header, IPV4_HEADER_MIN));
    return 0;
}
