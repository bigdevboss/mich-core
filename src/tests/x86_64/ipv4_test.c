#include "net_test.h"
#include "checksum.h"

struct arp_test_capture {
    u32 calls;
    u8 destination[6];
    u16 ether_type;
    u8 payload[ARP_PACKET_SIZE];
};

static int arp_test_transmit(const u8 destination[6], u16 ether_type,
                             const void *payload, u32 length, void *context) {
    struct arp_test_capture *capture = (struct arp_test_capture *)context;
    if (!capture || !destination || !payload || length != ARP_PACKET_SIZE)
        return -1;
    capture->calls++;
    for (u32 index = 0; index < 6; index++)
        capture->destination[index] = destination[index];
    capture->ether_type = ether_type;
    for (u32 index = 0; index < length; index++)
        capture->payload[index] = ((const u8 *)payload)[index];
    return 0;
}

int test_arp(void) {
    const u8 local_mac[6] = {0x02, 0x4D, 0x49, 0x43, 0x48, 0x02};
    const u8 peer_mac[6] = {0x02, 0x20, 0x30, 0x40, 0x50, 0x60};
    const u8 zero_mac[6] = {0, 0, 0, 0, 0, 0};
    struct ethernet_port port;
    struct arp_context arp;
    struct arp_test_capture capture;
    u8 *capture_bytes = (u8 *)&capture;
    for (usize_t index = 0; index < sizeof(capture); index++)
        capture_bytes[index] = 0;
    int valid = !ethernet_port_init(&port, local_mac) &&
        !arp_init(&arp, &port, 0xC0A8010A, 100, 10,
                  arp_test_transmit, &capture) &&
        arp_resolve(&arp, 0xC0A80101, 5) == 1 && capture.calls == 1 &&
        capture.ether_type == 0x0806 &&
        test_read_be16(capture.payload + 6) == 1 &&
        arp_cache_count(&arp, ARP_CACHE_PENDING) == 1 &&
        arp_resolve(&arp, 0xC0A80101, 5) == 1 && capture.calls == 1 &&
        arp.stats.rate_limited == 1;
    u8 frame[64];
    build_ethernet_frame(frame, local_mac, peer_mac, 0x0806, sizeof(frame));
    build_arp_payload(frame + 14, 2, peer_mac, 0xC0A80101,
                      local_mac, 0xC0A8010A);
    valid = valid && !ethernet_receive(&port, frame, sizeof(frame));
    u8 resolved[6];
    valid = valid && !arp_lookup(&arp, 0xC0A80101, resolved, 6);
    for (u32 index = 0; index < 6; index++)
        if (resolved[index] != peer_mac[index]) valid = 0;
    arp_tick(&arp, 106);
    valid = valid && arp_lookup(&arp, 0xC0A80101, resolved, 106) < 0;
    u32 calls_before = capture.calls;
    build_arp_payload(frame + 14, 1, peer_mac, 0xC0A80120,
                      zero_mac, 0xC0A8010A);
    valid = valid && !ethernet_receive(&port, frame, sizeof(frame)) &&
        capture.calls == calls_before + 1 &&
        test_read_be16(capture.payload + 6) == 2;
    for (u32 index = 0; index < 6; index++)
        if (capture.destination[index] != peer_mac[index]) valid = 0;
    calls_before = capture.calls;
    build_arp_payload(frame + 14, 1, peer_mac, 0xC0A80121,
                      zero_mac, 0xC0A80199);
    valid = valid && !ethernet_receive(&port, frame, sizeof(frame)) &&
        capture.calls == calls_before;
    build_arp_payload(frame + 14, 2, peer_mac, 0xC0A80122,
                      local_mac, 0xC0A8010A);
    frame[18] = 5;
    valid = valid && ethernet_receive(&port, frame, sizeof(frame)) < 0;
    build_arp_payload(frame + 14, 2, peer_mac, 0xC0A8010A,
                      local_mac, 0xC0A8010A);
    valid = valid && ethernet_receive(&port, frame, sizeof(frame)) < 0 &&
        arp.stats.conflicts == 1;
    for (u32 entry = 0; entry < 40; entry++) {
        u32 address = 0xC0A80201u + entry;
        build_ethernet_frame(frame, local_mac, peer_mac, 0x0806, sizeof(frame));
        build_arp_payload(frame + 14, 2, peer_mac, address,
                          local_mac, 0xC0A8010A);
        arp_tick(&arp, 200 + entry);
        if (ethernet_receive(&port, frame, sizeof(frame))) valid = 0;
    }
    valid = valid && arp_cache_count(&arp, ARP_CACHE_REACHABLE) <= ARP_CACHE_MAX &&
        !arp_lookup(&arp, 0xC0A80228, resolved, 240);
    u32 hot_objects = object_active_count();
    u32 hot_pages = pmm_free_pages();
    u64 start = test_cycles();
    for (u32 index = 0; index < 1024; index++) {
        u32 address = 0xC0A80301u + (index & 15);
        build_arp_payload(frame + 14, 2, peer_mac, address,
                          local_mac, 0xC0A8010A);
        if (ethernet_receive(&port, frame, sizeof(frame))) valid = 0;
    }
    u64 cycles = test_cycles() - start;
    valid = valid && cycles > 1024 && object_active_count() == hot_objects &&
        pmm_free_pages() == hot_pages && arp.stats.malformed >= 1 &&
        arp.stats.replies_received >= 1065 && arp.stats.cache_updates >= 1066;
    u32 reply_limited = arp.stats.rate_limited;
    u32 replies_sent = arp.stats.replies_sent;
    for (u32 index = 0; index < ARP_REPLY_MAX + 8; index++) {
        build_ethernet_frame(frame, local_mac, peer_mac, 0x0806, sizeof(frame));
        build_arp_payload(frame + 14, 1, peer_mac, 0xC0A80401u + index,
                          zero_mac, 0xC0A8010A);
        if (ethernet_receive(&port, frame, sizeof(frame))) valid = 0;
    }
    valid = valid && arp.stats.replies_sent == replies_sent + ARP_REPLY_MAX &&
        arp.stats.rate_limited == reply_limited + 8;
    if (valid) {
        serial64_write("Mich x86_64: ARP cycles/packet=0x");
        serial64_hex(cycles / 1024);
        serial64_write("\n");
    }
    return valid ? 0 : -1;
}

struct ipv4_test_context {
    u32 calls;
    u32 payload_bytes;
    u32 option_packets;
};

static int ipv4_test_handler(struct ipv4_context *ipv4,
                             const struct ipv4_packet_view *packet,
                             void *context) {
    (void)ipv4;
    struct ipv4_test_context *test =
        (struct ipv4_test_context *)context;
    if (!test || !packet) return -1;
    test->calls++;
    test->payload_bytes += packet->payload_length;
    if (packet->header_length > IPV4_HEADER_MIN) test->option_packets++;
    return 0;
}

int test_ipv4(void) {
    const u8 local_mac[6] = {0x02, 0x4D, 0x49, 0x43, 0x48, 0x03};
    const u8 peer_mac[6] = {0x02, 0x30, 0x40, 0x50, 0x60, 0x70};
    struct ethernet_port port;
    struct ipv4_context ipv4;
    struct ipv4_test_context udp = {0, 0, 0};
    int valid = !ethernet_port_init(&port, local_mac) &&
        !ipv4_init(&ipv4, &port, 0xC0A8010A, 0xFFFFFF00u, 1) &&
        !ipv4_register_protocol(&ipv4, 17, ipv4_test_handler, &udp) &&
        ipv4_register_protocol(&ipv4, 17, ipv4_test_handler, &udp) < 0;
    u8 packet[64];
    for (u32 index = 0; index < sizeof(packet); index++) packet[index] = 0;
    valid = valid && !ipv4_build_header(packet, sizeof(packet), 8,
                                        0xC0A80101, 0xC0A8010A,
                                        17, 64, 1, 1);
    for (u32 index = 20; index < 28; index++) packet[index] = (u8)index;
    valid = valid && !ipv4_receive(&ipv4, packet, 28);
    test_write_be32(packet + 16, 0xFFFFFFFFu);
    refresh_ipv4_checksum(packet);
    valid = valid && !ipv4_receive(&ipv4, packet, 28);
    test_write_be32(packet + 16, 0xC0A80144);
    refresh_ipv4_checksum(packet);
    valid = valid && ipv4_receive(&ipv4, packet, 28) < 0;
    test_write_be32(packet + 16, 0xC0A8010A);
    packet[0] = 0x65;
    refresh_ipv4_checksum(packet);
    valid = valid && ipv4_receive(&ipv4, packet, 28) < 0;
    packet[0] = 0x44;
    valid = valid && ipv4_receive(&ipv4, packet, 28) < 0;
    packet[0] = 0x45;
    test_write_be16(packet + 2, 19);
    refresh_ipv4_checksum(packet);
    valid = valid && ipv4_receive(&ipv4, packet, 28) < 0;
    test_write_be16(packet + 2, 40);
    refresh_ipv4_checksum(packet);
    valid = valid && ipv4_receive(&ipv4, packet, 28) < 0;
    test_write_be16(packet + 2, 28);
    refresh_ipv4_checksum(packet);
    packet[10] ^= 1;
    valid = valid && ipv4_receive(&ipv4, packet, 28) < 0;
    packet[10] ^= 1;
    packet[8] = 0;
    refresh_ipv4_checksum(packet);
    valid = valid && ipv4_receive(&ipv4, packet, 28) < 0;
    packet[8] = 64;
    test_write_be32(packet + 12, 0xE0000001u);
    refresh_ipv4_checksum(packet);
    valid = valid && ipv4_receive(&ipv4, packet, 28) < 0;
    test_write_be32(packet + 12, 0x7F000001u);
    refresh_ipv4_checksum(packet);
    valid = valid && ipv4_receive(&ipv4, packet, 28) < 0;
    test_write_be32(packet + 12, 0xC0A80101);
    test_write_be16(packet + 6, 0x2000);
    refresh_ipv4_checksum(packet);
    valid = valid && ipv4_receive(&ipv4, packet, 28) < 0;
    test_write_be16(packet + 6, 0x4000);
    packet[9] = 99;
    refresh_ipv4_checksum(packet);
    valid = valid && ipv4_receive(&ipv4, packet, 28) < 0;
    packet[0] = 0x46;
    packet[9] = 17;
    test_write_be16(packet + 2, 32);
    for (u32 index = 20; index < 24; index++) packet[index] = 1;
    refresh_ipv4_checksum(packet);
    valid = valid && !ipv4_receive(&ipv4, packet, 32);
    packet[0] = 0x45;
    test_write_be16(packet + 2, 28);
    refresh_ipv4_checksum(packet);
    u8 frame[64];
    build_ethernet_frame(frame, local_mac, peer_mac, 0x0800, sizeof(frame));
    for (u32 index = 0; index < 28; index++) frame[14 + index] = packet[index];
    valid = valid && !ethernet_receive(&port, frame, sizeof(frame));
    u32 hot_objects = object_active_count();
    u32 hot_pages = pmm_free_pages();
    u64 start = test_cycles();
    for (u32 index = 0; index < 1024; index++)
        if (ipv4_receive(&ipv4, packet, 28)) valid = 0;
    u64 cycles = test_cycles() - start;
    valid = valid && udp.calls == 1028 && udp.option_packets == 1 &&
        ipv4.stats.unicast == 1028 && ipv4.stats.broadcast == 1 &&
        ipv4.stats.drops_length >= 2 && ipv4.stats.drops_version >= 2 &&
        ipv4.stats.drops_checksum == 1 && ipv4.stats.drops_ttl == 1 &&
        ipv4.stats.drops_source == 2 && ipv4.stats.drops_destination == 1 &&
        ipv4.stats.drops_fragment == 1 && ipv4.stats.drops_protocol == 1 &&
        object_active_count() == hot_objects && pmm_free_pages() == hot_pages &&
        cycles > 1024;
    test_write_be32(packet + 12, 0xC0A8010A);
    test_write_be32(packet + 16, 0xC0A8010A);
    refresh_ipv4_checksum(packet);
    valid = valid && ipv4_receive(&ipv4, packet, 28) < 0 &&
        ipv4.stats.drops_land == 1;
    test_write_be32(packet + 12, 0xC0A8010A);
    test_write_be32(packet + 16, 0xFFFFFFFFu);
    refresh_ipv4_checksum(packet);
    valid = valid && ipv4_receive(&ipv4, packet, 28) < 0 &&
        ipv4.stats.drops_spoof == 1;
    {
        static const u32 lengths[] = {
            0, 1, 2, 3, 15, 16, 17, 31, 32, 63, 64, 65, 127, 128,
            511, 512, 1460, 1500
        };
        u8 src[1500];
        u8 dst[1500];
        for (u32 index = 0; index < sizeof(src); index++)
            src[index] = (u8)(index * 17 + 3);
        for (u32 n = 0; n < sizeof(lengths) / sizeof(lengths[0]); n++) {
            u32 length = lengths[n];
            u32 naive = 0;
            u32 index = 0;
            while (index + 1 < length) {
                naive += ((u32)src[index] << 8) | src[index + 1];
                index += 2;
            }
            if (index < length) naive += (u32)src[index] << 8;
            while (naive >> 16)
                naive = (naive & 0xFFFFu) + (naive >> 16);
            u16 expect = (u16)~naive;
            if (ipv4_checksum(src, length) != expect) valid = 0;
            if (net_checksum_finish(net_checksum_sum(0, src, length)) != expect)
                valid = 0;
            if (net_checksum_finish(net_checksum_copy(0, dst, src, length)) !=
                expect)
                valid = 0;
            for (index = 0; index < length; index++)
                if (dst[index] != src[index]) valid = 0;
        }
    }
    if (valid) {
        serial64_write("Mich x86_64: IPv4 cycles/packet=0x");
        serial64_hex(cycles / 1024);
        serial64_write("\n");
    }
    return valid ? 0 : -1;
}
