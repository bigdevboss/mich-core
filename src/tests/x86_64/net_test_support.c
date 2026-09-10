#include "net_test.h"

u64 test_cycles(void) {
    u32 lo;
    u32 hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((u64)hi << 32) | lo;
}

void build_ethernet_frame(u8 *frame, const u8 *destination,
                                 const u8 *source, u16 type, u32 length) {
    for (u32 index = 0; index < 6; index++) {
        frame[index] = destination[index];
        frame[6 + index] = source[index];
    }
    frame[12] = (u8)(type >> 8);
    frame[13] = (u8)type;
    for (u32 index = 14; index < length; index++) frame[index] = (u8)index;
}

void test_write_be16(u8 *bytes, u16 value) {
    bytes[0] = (u8)(value >> 8);
    bytes[1] = (u8)value;
}

void test_write_be32(u8 *bytes, u32 value) {
    bytes[0] = (u8)(value >> 24);
    bytes[1] = (u8)(value >> 16);
    bytes[2] = (u8)(value >> 8);
    bytes[3] = (u8)value;
}

u16 test_read_be16(const u8 *bytes) {
    return (u16)((u16)bytes[0] << 8) | bytes[1];
}

int icmp_test_transmit(struct ipv4_context *ipv4, u32 destination,
                              u8 protocol, const void *payload,
                              u32 payload_length, void *context) {
    struct icmp_test_link *link = (struct icmp_test_link *)context;
    if (!ipv4 || !link || !payload || destination != link->peer_address ||
        protocol != 1 || payload_length > 1480)
        return -1;
    u64 id = vnic_acquire_tx(link->vnic);
    u8 *page = (u8 *)packet_pool_data(link->pool, id, NET_BUFFER_TX);
    if (!id || !page) return -1;
    u32 offset = NET_PACKET_HEADROOM;
    for (u32 index = 0; index < 6; index++) {
        page[offset + index] = link->destination_mac[index];
        page[offset + 6 + index] = link->source_mac[index];
    }
    page[offset + 12] = 0x08;
    page[offset + 13] = 0x00;
    if (ipv4_build_header(page + offset + 14, 20, payload_length,
                          ipv4->local_address, destination, protocol,
                          64, link->identification++, 1)) {
        packet_pool_release(link->pool, id, NET_BUFFER_TX);
        return -1;
    }
    for (u32 index = 0; index < payload_length; index++)
        page[offset + 34 + index] = ((const u8 *)payload)[index];
    if (vnic_submit_tx(link->vnic, id, offset,
                       34 + payload_length, 0)) {
        packet_pool_release(link->pool, id, NET_BUFFER_TX);
        return -1;
    }
    return 0;
}

void build_icmp_echo(u8 *message, u32 length, u8 type,
                            u16 identifier, u16 sequence) {
    for (u32 index = 0; index < length; index++) message[index] = (u8)index;
    message[0] = type;
    message[1] = 0;
    message[2] = 0;
    message[3] = 0;
    test_write_be16(message + 4, identifier);
    test_write_be16(message + 6, sequence);
    u16 checksum = icmp_checksum(message, length);
    message[2] = (u8)(checksum >> 8);
    message[3] = (u8)checksum;
}

void refresh_ipv4_checksum(u8 *packet) {
    u32 header_length = (u32)(packet[0] & 0x0F) * 4;
    packet[10] = 0;
    packet[11] = 0;
    u16 checksum = ipv4_checksum(packet, header_length);
    packet[10] = (u8)(checksum >> 8);
    packet[11] = (u8)checksum;
}

void build_arp_payload(u8 *packet, u16 operation,
                              const u8 *sender_hardware,
                              u32 sender_protocol,
                              const u8 *target_hardware,
                              u32 target_protocol) {
    test_write_be16(packet, 1);
    test_write_be16(packet + 2, 0x0800);
    packet[4] = 6;
    packet[5] = 4;
    test_write_be16(packet + 6, operation);
    for (u32 index = 0; index < 6; index++) {
        packet[8 + index] = sender_hardware[index];
        packet[18 + index] = target_hardware[index];
    }
    test_write_be32(packet + 14, sender_protocol);
    test_write_be32(packet + 24, target_protocol);
}
