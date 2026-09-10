#include "checksum.h"
#include "icmpv6.h"

static u16 read_be16(const u8 *bytes) {
    return (u16)((u16)bytes[0] << 8) | bytes[1];
}

static u32 read_be32(const u8 *bytes) {
    return ((u32)bytes[0] << 24) | ((u32)bytes[1] << 16) |
           ((u32)bytes[2] << 8) | bytes[3];
}


u16 icmpv6_checksum(const u8 source[IPV6_ADDRESS_SIZE],
                    const u8 destination[IPV6_ADDRESS_SIZE],
                    const void *message, u32 length) {
    if (!source || !destination || !message || length < 4) return 0xFFFF;
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
    tail[7] = 58;
    sum = net_checksum_sum(sum, tail, sizeof(tail));
    sum = net_checksum_sum(sum, message, length);
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (u16)~sum;
}

static int mac_valid(const u8 mac[6]) {
    if (!mac || (mac[0] & 1)) return 0;
    u32 nonzero = 0;
    for (u32 index = 0; index < 6; index++) nonzero |= mac[index];
    return nonzero != 0;
}

static int update_neighbor(struct icmpv6_context *icmpv6,
                           const u8 address[IPV6_ADDRESS_SIZE],
                           const u8 mac[6], u32 state) {
    if (!icmpv6 || !address || !mac_valid(mac) ||
        ipv6_address_unspecified(address) || ipv6_address_multicast(address))
        return -1;
    u32 slot = ICMPV6_NEIGHBOR_MAX;
    u32 oldest = 0;
    for (u32 index = 0; index < ICMPV6_NEIGHBOR_MAX; index++) {
        struct icmpv6_neighbor *neighbor = &icmpv6->neighbors[index];
        if (neighbor->active &&
            ipv6_address_equal(neighbor->address, address)) {
            slot = index;
            break;
        }
        if (!neighbor->active) {
            slot = index;
            break;
        }
        if (slot == ICMPV6_NEIGHBOR_MAX ||
            (i32)(neighbor->updated - oldest) < 0) {
            slot = index;
            oldest = neighbor->updated;
        }
    }
    if (slot == ICMPV6_NEIGHBOR_MAX) return -1;
    struct icmpv6_neighbor *neighbor = &icmpv6->neighbors[slot];
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
        neighbor->address[index] = address[index];
    for (u32 index = 0; index < 6; index++) neighbor->mac[index] = mac[index];
    neighbor->state = state;
    neighbor->updated = icmpv6->now;
    neighbor->active = 1;
    icmpv6->stats.neighbor_updates++;
    return 0;
}

int icmpv6_neighbor_lookup(struct icmpv6_context *icmpv6,
                           const u8 address[IPV6_ADDRESS_SIZE],
                           u8 mac[6]) {
    if (!icmpv6 || !address || !mac) return -1;
    for (u32 index = 0; index < ICMPV6_NEIGHBOR_MAX; index++) {
        struct icmpv6_neighbor *neighbor = &icmpv6->neighbors[index];
        if (!neighbor->active ||
            !ipv6_address_equal(neighbor->address, address))
            continue;
        for (u32 byte = 0; byte < 6; byte++) mac[byte] = neighbor->mac[byte];
        return 0;
    }
    return -1;
}

struct ndp_options {
    const u8 *source_mac;
    const u8 *target_mac;
    const u8 *prefix;
    u32 prefix_valid;
    u32 prefix_preferred;
    u32 prefix_autonomous;
    u8 prefix_length;
};

static int parse_options(const u8 *bytes, u32 length,
                         struct ndp_options *options) {
    options->source_mac = 0;
    options->target_mac = 0;
    options->prefix = 0;
    options->prefix_valid = 0;
    options->prefix_preferred = 0;
    options->prefix_autonomous = 0;
    options->prefix_length = 0;
    u32 offset = 0;
    while (offset < length) {
        if (length - offset < 2 || !bytes[offset + 1]) return -1;
        u32 option_length = (u32)bytes[offset + 1] * 8;
        if (option_length > length - offset) return -1;
        u8 type = bytes[offset];
        if (type == ICMPV6_NDP_OPTION_SOURCE_LL) {
            if (option_length != 8 || options->source_mac) return -1;
            options->source_mac = bytes + offset + 2;
        } else if (type == ICMPV6_NDP_OPTION_TARGET_LL) {
            if (option_length != 8 || options->target_mac) return -1;
            options->target_mac = bytes + offset + 2;
        } else if (type == ICMPV6_NDP_OPTION_PREFIX) {
            if (option_length != 32 || options->prefix ||
                bytes[offset + 2] > 128)
                return -1;
            options->prefix_length = bytes[offset + 2];
            options->prefix_autonomous =
                (bytes[offset + 3] & 0x40) != 0;
            options->prefix_valid = read_be32(bytes + offset + 4);
            options->prefix_preferred = read_be32(bytes + offset + 8);
            if (options->prefix_preferred > options->prefix_valid) return -1;
            options->prefix = bytes + offset + 16;
        }
        offset += option_length;
    }
    return 0;
}

static int send_message(struct icmpv6_context *icmpv6,
                        const u8 source[IPV6_ADDRESS_SIZE],
                        const u8 destination[IPV6_ADDRESS_SIZE],
                        u8 *message, u32 length) {
    if (!icmpv6->transmit || !source) return -1;
    message[2] = 0;
    message[3] = 0;
    u16 checksum = icmpv6_checksum(
        source, destination, message, length);
    message[2] = (u8)(checksum >> 8);
    message[3] = (u8)checksum;
    if (icmpv6->transmit(source, destination, message, length,
                         icmpv6->transmit_context) < 0) {
        icmpv6->stats.transmit_errors++;
        return -1;
    }
    return 0;
}

static int reply_allowed(struct icmpv6_context *icmpv6) {
    if ((i32)(icmpv6->now - icmpv6->window_start) >=
        (i32)ICMPV6_REPLY_WINDOW) {
        icmpv6->window_start = icmpv6->now;
        icmpv6->replies_in_window = 0;
    }
    if (icmpv6->replies_in_window >= ICMPV6_REPLY_MAX) {
        icmpv6->stats.rate_limited++;
        return 0;
    }
    icmpv6->replies_in_window++;
    return 1;
}

static int handle_echo(struct icmpv6_context *icmpv6,
                       const struct ipv6_packet_view *packet) {
    if (packet->payload_length < 8 || packet->payload[1]) return -1;
    if (packet->payload[0] == ICMPV6_ECHO_REPLY) {
        icmpv6->stats.echo_replies++;
        return 0;
    }
    if (packet->payload[0] != ICMPV6_ECHO_REQUEST ||
        ipv6_address_unspecified(packet->source))
        return -1;
    icmpv6->stats.echo_requests++;
    if (!reply_allowed(icmpv6)) return 0;
    u8 reply[ICMPV6_MESSAGE_MAX];
    if (packet->payload_length > sizeof(reply)) return -1;
    net_copy(reply, packet->payload, packet->payload_length);
    reply[0] = ICMPV6_ECHO_REPLY;
    if (send_message(icmpv6, packet->destination,
                     packet->source, reply, packet->payload_length))
        return -1;
    icmpv6->stats.replies_sent++;
    return 0;
}

static int handle_ns(struct icmpv6_context *icmpv6,
                     const struct ipv6_packet_view *packet) {
    if (packet->hop_limit != 255 || packet->payload_length < 24 ||
        packet->payload[1])
        return -1;
    const u8 *target = packet->payload + 8;
    if (ipv6_address_multicast(target) || ipv6_address_unspecified(target))
        return -1;
    struct ndp_options options;
    if (parse_options(packet->payload + 24,
                      packet->payload_length - 24, &options)) {
        icmpv6->stats.option_errors++;
        return -1;
    }
    int unspecified = ipv6_address_unspecified(packet->source);
    if (unspecified && options.source_mac) return -1;
    if (!unspecified && options.source_mac)
        update_neighbor(icmpv6, packet->source, options.source_mac,
                        ICMPV6_NEIGHBOR_STALE);
    icmpv6->stats.neighbor_solicitations++;
    if (!ipv6_has_local_address(icmpv6->ipv6, target)) return 0;
    if (icmpv6->tentative &&
        ipv6_address_equal(target, icmpv6->local_address)) {
        icmpv6->duplicate = 1;
        icmpv6->stats.duplicate_addresses++;
        return 0;
    }
    if (!reply_allowed(icmpv6)) return 0;
    u8 destination[IPV6_ADDRESS_SIZE];
    if (unspecified) {
        for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
            destination[index] = 0;
        destination[0] = 0xFF;
        destination[1] = 0x02;
        destination[15] = 1;
    } else {
        for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
            destination[index] = packet->source[index];
    }
    u8 reply[32];
    for (u32 index = 0; index < sizeof(reply); index++) reply[index] = 0;
    reply[0] = ICMPV6_NEIGHBOR_ADVERTISEMENT;
    reply[4] = unspecified ? 0x20 : 0x60;
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
        reply[8 + index] = target[index];
    reply[24] = ICMPV6_NDP_OPTION_TARGET_LL;
    reply[25] = 1;
    for (u32 index = 0; index < 6; index++) reply[26 + index] = icmpv6->local_mac[index];
    return send_message(icmpv6, target, destination,
                        reply, sizeof(reply));
}

static int handle_na(struct icmpv6_context *icmpv6,
                     const struct ipv6_packet_view *packet) {
    if (packet->hop_limit != 255 || packet->payload_length < 24 ||
        packet->payload[1] || ipv6_address_unspecified(packet->source))
        return -1;
    const u8 *target = packet->payload + 8;
    if (ipv6_address_multicast(target) || ipv6_address_unspecified(target))
        return -1;
    struct ndp_options options;
    if (parse_options(packet->payload + 24,
                      packet->payload_length - 24, &options)) {
        icmpv6->stats.option_errors++;
        return -1;
    }
    if (!options.target_mac) return -1;
    if (icmpv6->tentative &&
        ipv6_address_equal(target, icmpv6->local_address)) {
        icmpv6->duplicate = 1;
        icmpv6->stats.duplicate_addresses++;
    }
    if (update_neighbor(icmpv6, target, options.target_mac,
                        ICMPV6_NEIGHBOR_REACHABLE))
        return -1;
    icmpv6->stats.neighbor_advertisements++;
    return 0;
}

static u32 lifetime_deadline(u32 now, u32 seconds) {
    if (!seconds) return 0;
    u32 maximum = 0x7FFFFFFFu / ICMPV6_TICKS_PER_SECOND;
    u32 ticks = seconds > maximum ? 0x7FFFFFFFu :
        seconds * ICMPV6_TICKS_PER_SECOND;
    return now + ticks;
}

static int handle_ra(struct icmpv6_context *icmpv6,
                     const struct ipv6_packet_view *packet) {
    if (packet->hop_limit != 255 || packet->payload_length < 16 ||
        packet->payload[1] || !ipv6_address_link_local(packet->source))
        return -1;
    struct ndp_options options;
    if (parse_options(packet->payload + 16,
                      packet->payload_length - 16, &options)) {
        icmpv6->stats.option_errors++;
        return -1;
    }
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
        icmpv6->router[index] = packet->source[index];
    icmpv6->router_lifetime = read_be16(packet->payload + 6);
    icmpv6->router_deadline = lifetime_deadline(
        icmpv6->now, icmpv6->router_lifetime);
    if (options.prefix) {
        for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
            icmpv6->prefix[index] = options.prefix[index];
        icmpv6->prefix_length = options.prefix_length;
        icmpv6->prefix_autonomous = options.prefix_autonomous;
        icmpv6->prefix_valid_lifetime = options.prefix_valid;
        icmpv6->prefix_preferred_lifetime = options.prefix_preferred;
        icmpv6->prefix_valid_deadline = lifetime_deadline(
            icmpv6->now, options.prefix_valid);
        icmpv6->prefix_preferred_deadline = lifetime_deadline(
            icmpv6->now, options.prefix_preferred);
    }
    if (options.source_mac)
        update_neighbor(icmpv6, packet->source, options.source_mac,
                        ICMPV6_NEIGHBOR_STALE);
    icmpv6->stats.router_advertisements++;
    return 0;
}

int icmpv6_ipv6_handler(struct ipv6_context *ipv6,
                        const struct ipv6_packet_view *packet,
                        void *context) {
    struct icmpv6_context *icmpv6 = context;
    if (!ipv6 || !packet || !icmpv6 || icmpv6->ipv6 != ipv6 ||
        packet->next_header != 58 || packet->payload_length < 4 ||
        packet->payload_length > ICMPV6_MESSAGE_MAX) {
        if (icmpv6) icmpv6->stats.malformed++;
        return -1;
    }
    if (icmpv6_checksum(packet->source, packet->destination,
                        packet->payload, packet->payload_length)) {
        icmpv6->stats.checksum_errors++;
        return -1;
    }
    u8 type = packet->payload[0];
    int result;
    if (type == ICMPV6_DESTINATION_UNREACHABLE &&
        packet->payload_length >= 8 && packet->payload[1] <= 6) {
        icmpv6->stats.destination_unreachable++;
        result = 0;
    } else if (type == ICMPV6_PACKET_TOO_BIG &&
               packet->payload_length >= 8 && !packet->payload[1]) {
        icmpv6->stats.packet_too_big++;
        if (packet->payload_length >= 8 + IPV6_HEADER_SIZE &&
            icmpv6->pmtu) {
            const u8 *quoted = packet->payload + 8;
            if ((quoted[0] >> 4) == 6 &&
                ipv6_has_local_address(ipv6, quoted + 8) &&
                !ipv6_address_multicast(quoted + 24) &&
                !ipv6_address_unspecified(quoted + 24))
                icmpv6->pmtu(quoted + 24, read_be32(packet->payload + 4),
                             icmpv6->pmtu_context);
        }
        result = 0;
    } else if (type == ICMPV6_TIME_EXCEEDED &&
               packet->payload_length >= 8 && packet->payload[1] <= 1) {
        icmpv6->stats.time_exceeded++;
        result = 0;
    } else if (type == ICMPV6_PARAMETER_PROBLEM &&
               packet->payload_length >= 8 && packet->payload[1] <= 2) {
        icmpv6->stats.parameter_problem++;
        result = 0;
    } else if (type == ICMPV6_ECHO_REQUEST || type == ICMPV6_ECHO_REPLY)
        result = handle_echo(icmpv6, packet);
    else if (type == ICMPV6_NEIGHBOR_SOLICITATION)
        result = handle_ns(icmpv6, packet);
    else if (type == ICMPV6_NEIGHBOR_ADVERTISEMENT)
        result = handle_na(icmpv6, packet);
    else if (type == ICMPV6_ROUTER_ADVERTISEMENT)
        result = handle_ra(icmpv6, packet);
    else if (type == ICMPV6_ROUTER_SOLICITATION &&
             packet->hop_limit == 255 && packet->payload_length >= 8 &&
             !packet->payload[1]) {
        icmpv6->stats.router_solicitations++;
        result = 0;
    } else {
        result = -1;
    }
    if (result < 0) {
        if ((type >= ICMPV6_ROUTER_SOLICITATION &&
             type <= ICMPV6_NEIGHBOR_ADVERTISEMENT) &&
            packet->hop_limit != 255)
            icmpv6->stats.hop_limit_errors++;
        else
            icmpv6->stats.malformed++;
    }
    return result;
}

int icmpv6_init(struct icmpv6_context *icmpv6, struct ipv6_context *ipv6,
                const u8 local_address[IPV6_ADDRESS_SIZE],
                const u8 local_mac[6], int tentative,
                icmpv6_transmit_fn transmit, void *transmit_context) {
    if (!icmpv6 || !ipv6 || !ipv6_address_link_local(local_address) ||
        !mac_valid(local_mac) || !transmit)
        return -1;
    icmpv6->ipv6 = ipv6;
    icmpv6->transmit = transmit;
    icmpv6->transmit_context = transmit_context;
    icmpv6->pmtu = 0;
    icmpv6->pmtu_context = 0;
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++) {
        icmpv6->local_address[index] = local_address[index];
        icmpv6->router[index] = 0;
        icmpv6->prefix[index] = 0;
    }
    for (u32 index = 0; index < 6; index++) icmpv6->local_mac[index] = local_mac[index];
    for (u32 index = 0; index < ICMPV6_NEIGHBOR_MAX; index++) {
        icmpv6->neighbors[index].state = 0;
        icmpv6->neighbors[index].updated = 0;
        icmpv6->neighbors[index].active = 0;
    }
    icmpv6->router_lifetime = 0;
    icmpv6->prefix_valid_lifetime = 0;
    icmpv6->prefix_preferred_lifetime = 0;
    icmpv6->router_deadline = 0;
    icmpv6->prefix_valid_deadline = 0;
    icmpv6->prefix_preferred_deadline = 0;
    icmpv6->prefix_length = 0;
    icmpv6->now = 0;
    icmpv6->window_start = 0;
    icmpv6->replies_in_window = 0;
    icmpv6->tentative = tentative != 0;
    icmpv6->duplicate = 0;
    icmpv6->prefix_autonomous = 0;
    u8 *stats = (u8 *)&icmpv6->stats;
    for (usize_t index = 0; index < sizeof(icmpv6->stats); index++) stats[index] = 0;
    return ipv6_register_protocol(ipv6, 58, icmpv6_ipv6_handler, icmpv6);
}

int icmpv6_build_router_solicitation(
    const u8 source[IPV6_ADDRESS_SIZE], const u8 mac[6],
    const u8 destination[IPV6_ADDRESS_SIZE], void *buffer,
    u32 buffer_length, u32 *message_length) {
    if (!source || !mac_valid(mac) || !destination || !buffer ||
        buffer_length < 16 || !message_length)
        return -1;
    u8 *message = buffer;
    for (u32 index = 0; index < 16; index++) message[index] = 0;
    message[0] = ICMPV6_ROUTER_SOLICITATION;
    message[8] = ICMPV6_NDP_OPTION_SOURCE_LL;
    message[9] = 1;
    for (u32 index = 0; index < 6; index++) message[10 + index] = mac[index];
    u16 checksum = icmpv6_checksum(source, destination, message, 16);
    message[2] = (u8)(checksum >> 8);
    message[3] = (u8)checksum;
    *message_length = 16;
    return 0;
}

int icmpv6_build_neighbor_solicitation(
    const u8 source[IPV6_ADDRESS_SIZE], const u8 target[IPV6_ADDRESS_SIZE],
    const u8 mac[6], void *buffer, u32 buffer_length,
    u8 destination[IPV6_ADDRESS_SIZE], u32 *message_length) {
    if (!source || !target || !mac || !buffer || !destination ||
        !message_length || ipv6_address_multicast(target) ||
        ipv6_address_unspecified(target))
        return -1;
    int dad = ipv6_address_unspecified(source);
    u32 length = dad ? 24 : 32;
    if (buffer_length < length || (!dad && !mac_valid(mac)) ||
        ipv6_solicited_node_address(target, destination))
        return -1;
    u8 *message = buffer;
    for (u32 index = 0; index < length; index++) message[index] = 0;
    message[0] = ICMPV6_NEIGHBOR_SOLICITATION;
    for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
        message[8 + index] = target[index];
    if (!dad) {
        message[24] = ICMPV6_NDP_OPTION_SOURCE_LL;
        message[25] = 1;
        for (u32 index = 0; index < 6; index++) message[26 + index] = mac[index];
    }
    u16 checksum = icmpv6_checksum(source, destination, message, length);
    message[2] = (u8)(checksum >> 8);
    message[3] = (u8)checksum;
    *message_length = length;
    return 0;
}

int icmpv6_set_pmtu_callback(struct icmpv6_context *icmpv6,
                             icmpv6_pmtu_fn function, void *context) {
    if (!icmpv6) return -1;
    icmpv6->pmtu = function;
    icmpv6->pmtu_context = context;
    return 0;
}

void icmpv6_tick(struct icmpv6_context *icmpv6, u32 now) {
    if (!icmpv6) return;
    icmpv6->now = now;
    if (icmpv6->router_deadline &&
        (i32)(now - icmpv6->router_deadline) >= 0) {
        icmpv6->router_lifetime = 0;
        icmpv6->router_deadline = 0;
        for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
            icmpv6->router[index] = 0;
    }
    if (icmpv6->prefix_preferred_deadline &&
        (i32)(now - icmpv6->prefix_preferred_deadline) >= 0) {
        icmpv6->prefix_preferred_lifetime = 0;
        icmpv6->prefix_preferred_deadline = 0;
    }
    if (icmpv6->prefix_valid_deadline &&
        (i32)(now - icmpv6->prefix_valid_deadline) >= 0) {
        icmpv6->prefix_valid_lifetime = 0;
        icmpv6->prefix_preferred_lifetime = 0;
        icmpv6->prefix_valid_deadline = 0;
        icmpv6->prefix_preferred_deadline = 0;
        icmpv6->prefix_autonomous = 0;
        icmpv6->prefix_length = 0;
        for (u32 index = 0; index < IPV6_ADDRESS_SIZE; index++)
            icmpv6->prefix[index] = 0;
    }
    for (u32 index = 0; index < ICMPV6_NEIGHBOR_MAX; index++) {
        struct icmpv6_neighbor *neighbor = &icmpv6->neighbors[index];
        if (!neighbor->active ||
            (i32)(now - neighbor->updated) < 30000)
            continue;
        neighbor->state = ICMPV6_NEIGHBOR_STALE;
    }
}
