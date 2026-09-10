#include "checksum.h"
#include "icmp.h"

static u16 read_be16(const u8 *bytes) {
    return (u16)((u16)bytes[0] << 8) | bytes[1];
}

static u32 read_be32(const u8 *bytes) {
    return ((u32)bytes[0] << 24) | ((u32)bytes[1] << 16) |
           ((u32)bytes[2] << 8) | bytes[3];
}

u16 icmp_checksum(const void *data, u32 length) {
    if (!data || !length) return 0xFFFF;
    return net_checksum_finish(net_checksum_sum(0, (const u8 *)data, length));
}

int icmp_init(struct icmp_context *icmp, struct ipv4_context *ipv4,
              u32 window_ticks, u32 max_replies_per_window) {
    if (!icmp || !ipv4 || !window_ticks || window_ticks > 0x7FFFFFFFu ||
        !max_replies_per_window)
        return -1;
    icmp->ipv4 = ipv4;
    icmp->pmtu = 0;
    icmp->pmtu_context = 0;
    icmp->now = 0;
    icmp->window_start = 0;
    icmp->window_ticks = window_ticks;
    icmp->replies_in_window = 0;
    icmp->max_replies_per_window = max_replies_per_window;
    u8 *stats = (u8 *)&icmp->stats;
    for (usize_t index = 0; index < sizeof(icmp->stats); index++) stats[index] = 0;
    return ipv4_register_protocol(ipv4, 1, icmp_ipv4_handler, icmp);
}

void icmp_tick(struct icmp_context *icmp, u32 now) {
    if (!icmp) return;
    icmp->now = now;
    if ((i32)(now - icmp->window_start) >= (i32)icmp->window_ticks) {
        icmp->window_start = now;
        icmp->replies_in_window = 0;
    }
}

static int reply_allowed(struct icmp_context *icmp) {
    icmp_tick(icmp, icmp->now);
    if (icmp->replies_in_window >= icmp->max_replies_per_window) {
        icmp->stats.rate_limited++;
        return 0;
    }
    icmp->replies_in_window++;
    return 1;
}

int icmp_ipv4_handler(struct ipv4_context *ipv4,
                      const struct ipv4_packet_view *packet,
                      void *context) {
    struct icmp_context *icmp = (struct icmp_context *)context;
    if (!ipv4 || !packet || !icmp || icmp->ipv4 != ipv4 ||
        packet->protocol != 1 || packet->payload_length < ICMP_HEADER_SIZE ||
        packet->payload_length > ICMP_MESSAGE_MAX) {
        if (icmp) icmp->stats.malformed++;
        return -1;
    }
    const u8 *message = packet->payload;
    if (icmp_checksum(message, packet->payload_length)) {
        icmp->stats.checksum_errors++;
        return -1;
    }
    u8 type = message[0];
    u8 code = message[1];
    if (type == ICMP_ECHO_REQUEST) {
        if (code) {
            icmp->stats.malformed++;
            return -1;
        }
        icmp->stats.echo_requests++;
        if (packet->destination == 0xFFFFFFFFu ||
            packet->destination == ipv4->subnet_broadcast) {
            icmp->stats.broadcast_suppressed++;
            return 0;
        }
        if (!reply_allowed(icmp)) return 0;
        u8 reply[ICMP_MESSAGE_MAX];
        net_copy(reply, message, packet->payload_length);
        reply[0] = ICMP_ECHO_REPLY;
        reply[2] = 0;
        reply[3] = 0;
        u16 checksum = icmp_checksum(reply, packet->payload_length);
        reply[2] = (u8)(checksum >> 8);
        reply[3] = (u8)checksum;
        if (ipv4_send(ipv4, packet->source, 1, reply,
                      packet->payload_length)) {
            icmp->stats.transmit_errors++;
            return -1;
        }
        icmp->stats.replies_sent++;
        return 0;
    }
    if (type == ICMP_ECHO_REPLY) {
        if (code) {
            icmp->stats.malformed++;
            return -1;
        }
        icmp->stats.echo_replies++;
        return 0;
    }
    if (type == ICMP_DESTINATION_UNREACHABLE) {
        if (code > 15) {
            icmp->stats.malformed++;
            return -1;
        }
        icmp->stats.destination_unreachable++;
        if (code == ICMP_FRAG_NEEDED &&
            packet->payload_length >= ICMP_HEADER_SIZE + IPV4_HEADER_MIN) {
            const u8 *quoted = message + ICMP_HEADER_SIZE;
            u32 header_length = (u32)(quoted[0] & 0x0F) * 4;
            if ((quoted[0] >> 4) == 4 &&
                header_length >= IPV4_HEADER_MIN &&
                packet->payload_length >= ICMP_HEADER_SIZE + header_length) {
                u32 quoted_source = read_be32(quoted + 12);
                u32 quoted_destination = read_be32(quoted + 16);
                if (quoted_source == ipv4->local_address &&
                    quoted_destination) {
                    icmp->stats.fragmentation_needed++;
                    if (icmp->pmtu)
                        icmp->pmtu(quoted_destination, read_be16(message + 6),
                                   icmp->pmtu_context);
                }
            }
        }
        return 0;
    }
    if (type == ICMP_TIME_EXCEEDED) {
        if (code > 1) {
            icmp->stats.malformed++;
            return -1;
        }
        icmp->stats.time_exceeded++;
        return 0;
    }
    icmp->stats.malformed++;
    return -1;
}

int icmp_send_port_unreachable(struct icmp_context *icmp,
                               const struct ipv4_packet_view *packet) {
    if (!icmp || !packet || packet->protocol != 17 ||
        packet->destination == 0xFFFFFFFFu ||
        packet->destination == icmp->ipv4->subnet_broadcast)
        return -1;
    if (!reply_allowed(icmp)) return -1;
    u32 quoted_payload = packet->payload_length < 8 ? packet->payload_length : 8;
    u32 quoted = packet->header_length + quoted_payload;
    if (quoted > 68) return -1;
    u8 message[76];
    for (u32 index = 0; index < 8 + quoted; index++) message[index] = 0;
    message[0] = ICMP_DESTINATION_UNREACHABLE;
    message[1] = 3;
    net_copy(message + 8, packet->packet, quoted);
    u16 checksum = icmp_checksum(message, 8 + quoted);
    message[2] = (u8)(checksum >> 8);
    message[3] = (u8)checksum;
    if (ipv4_send(icmp->ipv4, packet->source, 1,
                  message, 8 + quoted)) {
        icmp->stats.transmit_errors++;
        return -1;
    }
    icmp->stats.port_unreachable_sent++;
    return 0;
}

int icmp_set_pmtu_callback(struct icmp_context *icmp, icmp_pmtu_fn function,
                           void *context) {
    if (!icmp) return -1;
    icmp->pmtu = function;
    icmp->pmtu_context = context;
    return 0;
}
