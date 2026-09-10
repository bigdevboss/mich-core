#ifndef NET_TEST_H
#define NET_TEST_H

#include "types.h"
#include "object.h"
#include "pmm.h"
#include "resource.h"
#include "driver_supervisor.h"
#include "event.h"
#include "protos.h"
#include "vm64.h"
#include "ring.h"
#include "net_buffer.h"
#include "vnic.h"
#include "vnic_benchmark.h"
#include "net_interface.h"
#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "ipv6.h"
#include "icmp.h"
#include "icmpv6.h"
#include "udp.h"
#include "udpv6.h"
#include "tcp.h"
#include "pmtu.h"
#include "loopback.h"
#include "route.h"
#include "socket.h"
#include "serial64.h"
#include "tests64.h"

struct icmp_test_link {
    struct kernel_object *vnic;
    struct kernel_object *pool;
    u8 source_mac[6];
    u8 destination_mac[6];
    u32 peer_address;
    u16 identification;
};

u64 test_cycles(void);
void test_write_be16(u8 *bytes, u16 value);
void test_write_be32(u8 *bytes, u32 value);
u16 test_read_be16(const u8 *bytes);
void build_ethernet_frame(u8 *frame, const u8 *dst, const u8 *src,
                          u16 ether_type, u32 len);
void refresh_ipv4_checksum(u8 *packet);
void build_arp_payload(u8 *packet, u16 operation,
                       const u8 *sender_hardware, u32 sender_protocol,
                       const u8 *target_hardware, u32 target_protocol);
int icmp_test_transmit(struct ipv4_context *ipv4, u32 destination,
                       u8 protocol, const void *payload, u32 len, void *ctx);
void build_icmp_echo(u8 *msg, u32 len, u8 type, u16 id, u16 seq);

#endif
