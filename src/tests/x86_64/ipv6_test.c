#include "net_test.h"

static u32 ipv6_test_packets;
static u8 icmpv6_pmtu_dest[16];
static u32 icmpv6_pmtu_mtu;
static u32 icmpv6_pmtu_calls;

static void icmpv6_test_pmtu(const u8 destination[16], u32 mtu, void *context) {
    (void)context;
    icmpv6_pmtu_calls++;
    icmpv6_pmtu_mtu = mtu;
    for (u32 index = 0; index < 16; index++)
        icmpv6_pmtu_dest[index] = destination[index];
}

static int ipv6_test_handler(struct ipv6_context *ipv6,
                             const struct ipv6_packet_view *packet,
                             void *context) {
    if (!ipv6 || !packet || context != &ipv6_test_packets ||
        packet->next_header != 17)
        return -1;
    ipv6_test_packets++;
    return 0;
}

int test_ipv6(void) {
    const u8 mac[6] = {0x02, 0x4D, 0x49, 0x43, 0x48, 0x66};
    const u8 local[16] = {
        0xFE, 0x80, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 2
    };
    const u8 peer[16] = {
        0xFE, 0x80, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 1
    };
    const u8 multicast[16] = {
        0xFF, 0x02, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 1
    };
    struct ethernet_port port;
    struct ipv6_context ipv6;
    ipv6_test_packets = 0;
    int valid = !ethernet_port_init(&port, mac) &&
        !ipv6_init(&ipv6, &port, local, 1) &&
        !ipv6_register_protocol(&ipv6, 17, ipv6_test_handler,
                                &ipv6_test_packets) &&
        ipv6_register_protocol(&ipv6, 17, ipv6_test_handler,
                               &ipv6_test_packets) < 0 &&
        ipv6_address_link_local(local) &&
        ipv6_address_multicast(multicast) &&
        !ipv6_address_unspecified(local) &&
        ipv6_address_equal(local, local) &&
        !ipv6_address_equal(local, peer);
    u8 packet[64];
    for (u32 index = 0; index < sizeof(packet); index++) packet[index] = 0;
    valid = valid && !ipv6_build_header(packet, sizeof(packet), 8,
                                        peer, local, 17, 64, 0x2E, 0x12345);
    for (u32 index = 0; index < 8; index++) packet[40 + index] = (u8)index;
    struct ipv6_packet_view view;
    valid = valid && !ipv6_parse(packet, 48, &view) &&
        view.packet_length == 48 && view.payload_offset == 40 &&
        view.payload_length == 8 && view.next_header == 17 &&
        view.hop_limit == 64 && view.traffic_class == 0x2E &&
        view.flow_label == 0x12345 && !ipv6_receive(&ipv6, packet, 48) &&
        ipv6_test_packets == 1;
    u8 extended[64];
    for (u32 index = 0; index < sizeof(extended); index++) extended[index] = 0;
    valid = valid && !ipv6_build_header(extended, sizeof(extended), 16,
                                        peer, local, 60, 32, 0, 0);
    extended[40] = 17;
    extended[41] = 0;
    valid = valid && !ipv6_parse(extended, 56, &view) &&
        view.extension_count == 1 && view.payload_offset == 48 &&
        view.payload_length == 8 && !ipv6_receive(&ipv6, extended, 56) &&
        ipv6_test_packets == 2;
    valid = valid && !ipv6_build_header(packet, sizeof(packet), 8,
                                        peer, multicast, 17, 255, 0, 0) &&
        !ipv6_receive(&ipv6, packet, 48) && ipv6_test_packets == 3;
    valid = valid && !ipv6_build_header(packet, sizeof(packet), 8,
                                        peer, local, 44, 64, 0, 0) &&
        ipv6_parse(packet, 48, &view) == -6;
    valid = valid && !ipv6_build_header(packet, sizeof(packet), 8,
                                        peer, local, 43, 64, 0, 0) &&
        ipv6_parse(packet, 48, &view) == -5;
    packet[0] = 0x40;
    valid = valid && ipv6_parse(packet, 48, &view) == -2;
    packet[0] = 0x60;
    packet[7] = 0;
    valid = valid && ipv6_parse(packet, 48, &view) == -3;
    packet[7] = 64;
    packet[4] = 0xFF;
    packet[5] = 0xFF;
    valid = valid && ipv6_parse(packet, 48, &view) == -1 &&
        ipv6_build_header(packet, sizeof(packet), 8,
                          multicast, local, 17, 64, 0, 0) < 0 &&
        ipv6.stats.unicast == 2 && ipv6.stats.multicast == 1;
    valid = valid && !ipv6_build_header(packet, sizeof(packet), 8,
                                        peer, peer, 17, 64, 0, 0) &&
        ipv6_receive(&ipv6, packet, 48) < 0 && ipv6.stats.drops_land == 1;
    valid = valid && !ipv6_build_header(packet, sizeof(packet), 8,
                                        local, peer, 17, 64, 0, 0) &&
        ipv6_receive(&ipv6, packet, 48) < 0 && ipv6.stats.drops_spoof == 1;
    u8 unspecified[16];
    for (u32 index = 0; index < 16; index++) unspecified[index] = 0;
    valid = valid && !ipv6_build_header(packet, sizeof(packet), 8,
                                        unspecified, local, 17, 64, 0, 0) &&
        ipv6_receive(&ipv6, packet, 48) < 0 && ipv6.stats.drops_source >= 1;
    return valid ? 0 : -1;
}

struct icmpv6_test_link {
    u8 destination[16];
    u8 message[128];
    u32 length;
    u32 transmitted;
};

static int icmpv6_test_transmit(const u8 source[16],
                                const u8 destination[16],
                                const void *message, u32 length,
                                void *context) {
    struct icmpv6_test_link *link = context;
    if (!source || !destination || !message || !link ||
        length > sizeof(link->message))
        return -1;
    for (u32 index = 0; index < 16; index++)
        link->destination[index] = destination[index];
    for (u32 index = 0; index < length; index++)
        link->message[index] = ((const u8 *)message)[index];
    link->length = length;
    link->transmitted++;
    return 0;
}

int test_icmpv6(void) {
    const u8 local_mac[6] = {0x02, 0x4D, 0x49, 0x43, 0x48, 0x67};
    const u8 peer_mac[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0x67};
    u8 local[16];
    u8 peer[16];
    u8 solicited[16];
    int valid = !ipv6_link_local_from_mac(local_mac, local) &&
        !ipv6_link_local_from_mac(peer_mac, peer) &&
        !ipv6_solicited_node_address(local, solicited) &&
        local[0] == 0xFE && local[1] == 0x80 &&
        local[8] == (u8)(local_mac[0] ^ 2) &&
        local[11] == 0xFF && local[12] == 0xFE &&
        solicited[0] == 0xFF && solicited[1] == 0x02 &&
        solicited[11] == 1 && solicited[12] == 0xFF &&
        solicited[15] == local[15];
    struct ethernet_port port;
    struct ipv6_context ipv6;
    struct icmpv6_context icmpv6;
    struct icmpv6_test_link link;
    link.length = 0;
    link.transmitted = 0;
    valid = valid && !ethernet_port_init(&port, local_mac) &&
        !ipv6_init(&ipv6, &port, local, 1) &&
        !icmpv6_init(&icmpv6, &ipv6, local, local_mac, 0,
                     icmpv6_test_transmit, &link);
    u8 packet[128];
    for (u32 index = 0; index < sizeof(packet); index++) packet[index] = 0;
    valid = valid && !ipv6_build_header(packet, sizeof(packet), 16,
                                        peer, local, 58, 64, 0, 0);
    packet[40] = ICMPV6_ECHO_REQUEST;
    packet[44] = 0x4D;
    packet[45] = 0x49;
    packet[47] = 1;
    for (u32 index = 8; index < 16; index++) packet[40 + index] = (u8)index;
    u16 checksum = icmpv6_checksum(peer, local, packet + 40, 16);
    packet[42] = (u8)(checksum >> 8);
    packet[43] = (u8)checksum;
    valid = valid && !ipv6_receive(&ipv6, packet, 56) &&
        link.transmitted == 1 && link.length == 16 &&
        link.message[0] == ICMPV6_ECHO_REPLY &&
        ipv6_address_equal(link.destination, peer) &&
        !icmpv6_checksum(local, peer, link.message, link.length) &&
        icmpv6.stats.echo_requests == 1 && icmpv6.stats.replies_sent == 1;
    u8 ns[32];
    u32 ns_length;
    u8 ns_destination[16];
    valid = valid && !icmpv6_build_neighbor_solicitation(
        peer, local, peer_mac, ns, sizeof(ns), ns_destination, &ns_length) &&
        ns_length == 32 && ipv6_address_equal(ns_destination, solicited) &&
        !ipv6_build_header(packet, sizeof(packet), ns_length,
                           peer, ns_destination, 58, 255, 0, 0);
    for (u32 index = 0; index < ns_length; index++) packet[40 + index] = ns[index];
    valid = valid && !ipv6_receive(&ipv6, packet, 40 + ns_length) &&
        link.transmitted == 2 && link.message[0] ==
        ICMPV6_NEIGHBOR_ADVERTISEMENT &&
        ipv6_address_equal(link.destination, peer) &&
        icmpv6.stats.neighbor_solicitations == 1;
    u8 learned[6];
    valid = valid && !icmpv6_neighbor_lookup(&icmpv6, peer, learned);
    for (u32 index = 0; index < 6; index++)
        if (learned[index] != peer_mac[index]) valid = 0;
    u8 all_nodes[16];
    for (u32 index = 0; index < 16; index++) all_nodes[index] = 0;
    all_nodes[0] = 0xFF;
    all_nodes[1] = 0x02;
    all_nodes[15] = 1;
    for (u32 index = 0; index < sizeof(packet); index++) packet[index] = 0;
    valid = valid && !ipv6_build_header(packet, sizeof(packet), 56,
                                        peer, all_nodes, 58, 255, 0, 0);
    packet[40] = ICMPV6_ROUTER_ADVERTISEMENT;
    packet[44] = 64;
    packet[46] = 0x07;
    packet[47] = 0x08;
    packet[56] = ICMPV6_NDP_OPTION_SOURCE_LL;
    packet[57] = 1;
    for (u32 index = 0; index < 6; index++) packet[58 + index] = peer_mac[index];
    packet[64] = ICMPV6_NDP_OPTION_PREFIX;
    packet[65] = 4;
    packet[66] = 64;
    packet[67] = 0xC0;
    packet[68] = 0;
    packet[69] = 0;
    packet[70] = 0x0E;
    packet[71] = 0x10;
    packet[72] = 0;
    packet[73] = 0;
    packet[74] = 0x07;
    packet[75] = 0x08;
    packet[80] = 0xFD;
    checksum = icmpv6_checksum(peer, all_nodes, packet + 40, 56);
    packet[42] = (u8)(checksum >> 8);
    packet[43] = (u8)checksum;
    valid = valid && !ipv6_receive(&ipv6, packet, 96) &&
        icmpv6.stats.router_advertisements == 1 &&
        icmpv6.router_lifetime == 1800 && icmpv6.prefix_length == 64 &&
        icmpv6.prefix[0] == 0xFD &&
        icmpv6.prefix_valid_lifetime == 3600 &&
        icmpv6.prefix_preferred_lifetime == 1800;
    struct ethernet_port dad_port;
    struct ipv6_context dad_ipv6;
    struct icmpv6_context dad;
    struct icmpv6_test_link dad_link;
    dad_link.length = 0;
    dad_link.transmitted = 0;
    valid = valid && !ethernet_port_init(&dad_port, local_mac) &&
        !ipv6_init(&dad_ipv6, &dad_port, local, 1) &&
        !icmpv6_init(&dad, &dad_ipv6, local, local_mac, 1,
                     icmpv6_test_transmit, &dad_link);
    u8 unspecified[16];
    for (u32 index = 0; index < 16; index++) unspecified[index] = 0;
    valid = valid && !icmpv6_build_neighbor_solicitation(
        unspecified, local, local_mac, ns, sizeof(ns),
        ns_destination, &ns_length) && ns_length == 24 &&
        !ipv6_build_header(packet, sizeof(packet), ns_length,
                           peer, ns_destination, 58, 255, 0, 0);
    for (u32 index = 0; index < ns_length; index++) packet[40 + index] = ns[index];
    packet[42] = 0;
    packet[43] = 0;
    checksum = icmpv6_checksum(unspecified, ns_destination,
                               packet + 40, ns_length);
    packet[42] = (u8)(checksum >> 8);
    packet[43] = (u8)checksum;
    for (u32 index = 0; index < 16; index++) packet[8 + index] = 0;
    valid = valid && !ipv6_receive(&dad_ipv6, packet, 40 + ns_length) &&
        dad.duplicate && dad.stats.duplicate_addresses == 1 &&
        !dad_link.transmitted;
    u8 rs[16];
    u32 rs_length;
    valid = valid && !icmpv6_build_router_solicitation(
        local, local_mac, all_nodes, rs, sizeof(rs), &rs_length) &&
        rs_length == 16 && !icmpv6_checksum(local, all_nodes, rs, rs_length);
    icmpv6_pmtu_calls = 0;
    icmpv6_pmtu_mtu = 0;
    valid = valid && !icmpv6_set_pmtu_callback(&icmpv6, icmpv6_test_pmtu, 0);
    u8 quoted_dest[16];
    for (u32 index = 0; index < 16; index++) quoted_dest[index] = 0;
    quoted_dest[0] = 0xFD;
    quoted_dest[15] = 4;
    valid = valid && !ipv6_build_header(packet, sizeof(packet),
                                        8 + IPV6_HEADER_SIZE,
                                        peer, local, 58, 64, 0, 0);
    packet[40] = ICMPV6_PACKET_TOO_BIG;
    packet[41] = 0;
    packet[42] = 0;
    packet[43] = 0;
    packet[44] = 0;
    packet[45] = 0;
    packet[46] = 0x05;
    packet[47] = 0x00;
    valid = valid && !ipv6_build_header(packet + 48, sizeof(packet) - 48, 0,
                                        local, quoted_dest, 6, 64, 0, 0);
    checksum = icmpv6_checksum(peer, local, packet + 40,
                               8 + IPV6_HEADER_SIZE);
    packet[42] = (u8)(checksum >> 8);
    packet[43] = (u8)checksum;
    valid = valid && !ipv6_receive(&ipv6, packet,
                                   40 + 8 + IPV6_HEADER_SIZE) &&
        icmpv6.stats.packet_too_big == 1 && icmpv6_pmtu_calls == 1 &&
        icmpv6_pmtu_mtu == 1280 &&
        ipv6_address_equal(icmpv6_pmtu_dest, quoted_dest);
    struct pmtu_cache cache6;
    pmtu_init(&cache6, 50);
    valid = valid && !pmtu_update6(&cache6, quoted_dest, 1280, 1500) &&
        pmtu_lookup6(&cache6, quoted_dest, 1500) == 1280;
    return valid ? 0 : -1;
}
