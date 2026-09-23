#include "net_test.h"
#include "vfs.h"

static int unreachable(const struct ipv4_packet_view *packet, void *ctx) {
    return icmp_send_port_unreachable((struct icmp_context *)ctx, packet);
}

static u32 icmp_pmtu_dest;
static u32 icmp_pmtu_mtu;
static u32 icmp_pmtu_calls;

static void icmp_test_pmtu(u32 destination, u32 mtu, void *context) {
    (void)context;
    icmp_pmtu_calls++;
    icmp_pmtu_dest = destination;
    icmp_pmtu_mtu = mtu;
}

int test_icmp(void) {
    u32 free_pages = pmm_free_pages();
    u32 objects = object_active_count();
    u32 pools = packet_pool_active_count();
    u32 vnics = vnic_active_count();
    u32 rings = ring_active_count();
    struct kernel_object *vnic = vnic_create(32, 16);
    struct kernel_object *pool = vnic_pool(vnic);
    if (!vnic || !pool) {
        if (vnic) object_release(vnic);
        return -1;
    }
    const u8 local_mac[6] = {0x02, 0x4D, 0x49, 0x43, 0x48, 0x04};
    const u8 peer_mac[6] = {0x02, 0x40, 0x50, 0x60, 0x70, 0x80};
    struct ethernet_port port;
    struct ipv4_context ipv4;
    struct icmp_context icmp;
    struct icmp_test_link link;
    link.vnic = vnic;
    link.pool = pool;
    link.peer_address = 0xC0A80101;
    link.identification = 1;
    for (u32 index = 0; index < 6; index++) {
        link.source_mac[index] = local_mac[index];
        link.destination_mac[index] = peer_mac[index];
    }
    int valid = !ethernet_port_init(&port, local_mac) &&
        !ipv4_init(&ipv4, &port, 0xC0A8010A, 0xFFFFFF00u, 1) &&
        !ipv4_set_transmit(&ipv4, icmp_test_transmit, &link) &&
        !icmp_init(&icmp, &ipv4, 10, 2);
    u8 message[32];
    build_icmp_echo(message, sizeof(message), ICMP_ECHO_REQUEST, 0x1234, 1);
    u8 frame[96];
    build_ethernet_frame(frame, local_mac, peer_mac, 0x0800, sizeof(frame));
    ipv4_build_header(frame + 14, 20, sizeof(message),
                      link.peer_address, ipv4.local_address, 1, 64, 1, 1);
    for (u32 index = 0; index < sizeof(message); index++)
        frame[34 + index] = message[index];
    valid = valid && !vnic_inject(vnic, frame, 66) &&
        !ethernet_receive_vnic(&port, vnic);
    struct net_packet_descriptor reply_descriptor;
    valid = valid && !vnic_drain_tx(vnic, &reply_descriptor);
    struct page_resource *backing = page_resource_get(packet_pool_backing(pool));
    u32 reply_slot = (u32)reply_descriptor.buffer_id - 1;
    u8 *reply = backing && reply_slot < backing->pages ?
        (u8 *)(uptr_t)backing->physical[reply_slot] + reply_descriptor.offset : 0;
    struct ipv4_packet_view reply_ip;
    valid = valid && reply && reply_descriptor.length == 66 &&
        !ipv4_parse(reply + 14, reply_descriptor.length - 14, &reply_ip) &&
        reply_ip.source == ipv4.local_address &&
        reply_ip.destination == link.peer_address && reply_ip.protocol == 1 &&
        reply_ip.payload_length == sizeof(message) &&
        reply_ip.payload[0] == ICMP_ECHO_REPLY && !reply_ip.payload[1] &&
        !icmp_checksum(reply_ip.payload, reply_ip.payload_length) &&
        test_read_be16(reply_ip.payload + 4) == 0x1234 &&
        test_read_be16(reply_ip.payload + 6) == 1;
    frame[36] ^= 1;
    valid = valid && !vnic_inject(vnic, frame, 66) &&
        ethernet_receive_vnic(&port, vnic) < 0;
    frame[36] ^= 1;
    test_write_be32(frame + 30, 0xFFFFFFFFu);
    refresh_ipv4_checksum(frame + 14);
    build_ethernet_frame(frame, (const u8[]){0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
                         peer_mac, 0x0800, sizeof(frame));
    ipv4_build_header(frame + 14, 20, sizeof(message),
                      link.peer_address, 0xFFFFFFFFu, 1, 64, 2, 1);
    for (u32 index = 0; index < sizeof(message); index++)
        frame[34 + index] = message[index];
    valid = valid && !ethernet_receive(&port, frame, 66) &&
        vnic_drain_tx(vnic, &reply_descriptor) < 0 &&
        icmp.stats.broadcast_suppressed == 1;
    icmp_tick(&icmp, 20);
    build_ethernet_frame(frame, local_mac, peer_mac, 0x0800, sizeof(frame));
    ipv4_build_header(frame + 14, 20, sizeof(message),
                      link.peer_address, ipv4.local_address, 1, 64, 3, 1);
    for (u32 index = 0; index < sizeof(message); index++)
        frame[34 + index] = message[index];
    for (u32 index = 0; index < 3; index++)
        if (ethernet_receive(&port, frame, 66)) valid = 0;
    u32 drained = 0;
    while (!vnic_drain_tx(vnic, &reply_descriptor)) drained++;
    valid = valid && drained == 2 && icmp.stats.rate_limited == 1;
    u8 control[8] = {ICMP_DESTINATION_UNREACHABLE, 0, 0, 0, 0, 0, 0, 0};
    u16 checksum = icmp_checksum(control, sizeof(control));
    control[2] = (u8)(checksum >> 8);
    control[3] = (u8)checksum;
    struct ipv4_packet_view control_packet;
    u8 control_ip[28];
    ipv4_build_header(control_ip, sizeof(control_ip), sizeof(control),
                      link.peer_address, ipv4.local_address, 1, 64, 4, 1);
    for (u32 index = 0; index < sizeof(control); index++)
        control_ip[20 + index] = control[index];
    valid = valid && !ipv4_parse(control_ip, sizeof(control_ip), &control_packet) &&
        !icmp_ipv4_handler(&ipv4, &control_packet, &icmp);
    u32 hot_pages = pmm_free_pages();
    u32 hot_objects = object_active_count();
    icmp.max_replies_per_window = 2048;
    icmp_tick(&icmp, 40);
    u64 start = test_cycles();
    for (u32 index = 0; index < 1024; index++) {
        if (ethernet_receive(&port, frame, 66) ||
            vnic_drain_tx(vnic, &reply_descriptor))
            valid = 0;
    }
    u64 cycles = test_cycles() - start;
    valid = valid && cycles > 1024 && pmm_free_pages() == hot_pages &&
        object_active_count() == hot_objects && packet_pool_free_count(pool) == 32 &&
        icmp.stats.echo_requests == 1029 && icmp.stats.replies_sent == 1027 &&
        icmp.stats.checksum_errors == 1 && icmp.stats.destination_unreachable == 1;
    icmp_pmtu_dest = 0;
    icmp_pmtu_mtu = 0;
    icmp_pmtu_calls = 0;
    valid = valid && !icmp_set_pmtu_callback(&icmp, icmp_test_pmtu, 0);
    u8 quoted[20];
    u8 ptb[28];
    u8 ptb_ip[48];
    for (u32 index = 0; index < sizeof(ptb); index++) ptb[index] = 0;
    valid = valid && !ipv4_build_header(quoted, sizeof(quoted), 100,
                                        ipv4.local_address, 0x0A000002u,
                                        6, 64, 9, 1);
    ptb[0] = ICMP_DESTINATION_UNREACHABLE;
    ptb[1] = ICMP_FRAG_NEEDED;
    ptb[6] = 0x05;
    ptb[7] = 0x00;
    for (u32 index = 0; index < 20; index++) ptb[8 + index] = quoted[index];
    checksum = icmp_checksum(ptb, sizeof(ptb));
    ptb[2] = (u8)(checksum >> 8);
    ptb[3] = (u8)checksum;
    valid = valid && !ipv4_build_header(ptb_ip, sizeof(ptb_ip), sizeof(ptb),
                                        link.peer_address, ipv4.local_address,
                                        1, 64, 10, 1);
    for (u32 index = 0; index < sizeof(ptb); index++)
        ptb_ip[20 + index] = ptb[index];
    valid = valid && !ipv4_receive(&ipv4, ptb_ip, 48) &&
        icmp.stats.fragmentation_needed == 1 && icmp_pmtu_calls == 1 &&
        icmp_pmtu_dest == 0x0A000002u && icmp_pmtu_mtu == 1280;
    struct pmtu_cache cache;
    pmtu_init(&cache, 100);
    valid = valid && !pmtu_update4(&cache, 0x0A000002u, 1280, 1500) &&
        pmtu_lookup4(&cache, 0x0A000002u, 1500) == 1280 &&
        pmtu_update4(&cache, 0x0A000002u, 1400, 1500) == 1 &&
        pmtu_lookup4(&cache, 0x0A000002u, 1500) == 1280 &&
        !pmtu_update4(&cache, 0x0A000002u, 576, 1500) &&
        pmtu_lookup4(&cache, 0x0A000002u, 1500) == 576;
    pmtu_tick(&cache, 100);
    valid = valid && pmtu_lookup4(&cache, 0x0A000002u, 1500) == 1500 &&
        cache.expired >= 1;
    if (valid) {
        serial64_write("Mich x86_64: ICMP echo cycles/packet=0x");
        serial64_hex(cycles / 1024);
        serial64_write("\n");
    }
    vnic_revoke(vnic);
    object_release(vnic);
    valid = valid && pmm_free_pages() == free_pages &&
        object_active_count() == objects && packet_pool_active_count() == pools &&
        vnic_active_count() == vnics && ring_active_count() == rings;
    return valid ? 0 : -1;
}

int test_loopback(void) {
    u32 free_pages = pmm_free_pages();
    u32 objects = object_active_count();
    u32 pools = packet_pool_active_count();
    const u8 loop_mac[6] = {0x02, 0x4D, 0x49, 0x43, 0x48, 0x7F};
    static struct ethernet_port port;
    static struct ipv4_context ipv4;
    static struct icmp_context icmp;
    static struct loopback_context loopback;
    int valid = !ethernet_port_init(&port, loop_mac) &&
        !ipv4_init(&ipv4, &port, 0x7F000001u, 0xFF000000u, 0) &&
        !loopback_init(&loopback, &ipv4, 8) &&
        !icmp_init(&icmp, &ipv4, 100, 2048);
    u8 echo[32];
    build_icmp_echo(echo, sizeof(echo), ICMP_ECHO_REQUEST, 0x4D49, 1);
    valid = valid && !ipv4_send(&ipv4, 0x7F000001u, 1,
                                echo, sizeof(echo)) &&
        icmp.stats.echo_requests == 1 && icmp.stats.replies_sent == 1 &&
        icmp.stats.echo_replies == 1 && loopback.stats.transmitted == 2 &&
        loopback.stats.received == 2 &&
        ipv4_send(&ipv4, 0xC0A80101u, 1, echo, sizeof(echo)) < 0 &&
        ipv4_send(&ipv4, 0x7F000001u, 1, echo, LOOPBACK_MTU + 1) < 0 &&
        ipv4_send(&ipv4, 0x7F000001u, 99, echo, sizeof(echo)) < 0;
    u32 hot_pages = pmm_free_pages();
    u32 hot_objects = object_active_count();
    icmp_tick(&icmp, 200);
    u64 start = test_cycles();
    for (u32 index = 0; index < 1024; index++) {
        echo[6] = (u8)(index >> 8);
        echo[7] = (u8)index;
        echo[2] = 0;
        echo[3] = 0;
        u16 checksum = icmp_checksum(echo, sizeof(echo));
        echo[2] = (u8)(checksum >> 8);
        echo[3] = (u8)checksum;
        if (ipv4_send(&ipv4, 0x7F000001u, 1,
                      echo, sizeof(echo)))
            valid = 0;
    }
    u64 cycles = test_cycles() - start;
    valid = valid && cycles > 1024 && icmp.stats.echo_requests == 1025 &&
        icmp.stats.replies_sent == 1025 && icmp.stats.echo_replies == 1025 &&
        loopback.stats.transmitted == 2051 && loopback.stats.received == 2050 &&
        loopback.stats.drops == 3 && packet_pool_free_count(loopback.pool) == 8 &&
        pmm_free_pages() == hot_pages && object_active_count() == hot_objects;
    if (valid) {
        serial64_write("Mich x86_64: loopback ping cycles/packet=0x");
        serial64_hex(cycles / 1024);
        serial64_write("\n");
    }
    loopback_destroy(&loopback);
    valid = valid && !ipv4.transmit && !ipv4.transmit_context &&
        pmm_free_pages() == free_pages && object_active_count() == objects &&
        packet_pool_active_count() == pools;
    return valid ? 0 : -1;
}

int test_udp(void) {
    u32 free_pages = pmm_free_pages();
    u32 objects = object_active_count();
    u32 pools = packet_pool_active_count();
    const u8 loop_mac[6] = {0x02, 0x4D, 0x49, 0x43, 0x48, 0x55};
    static struct ethernet_port port;
    static struct ipv4_context ipv4;
    static struct loopback_context loopback;
    static struct icmp_context icmp;
    static struct udp_context udp;
    int valid = !ethernet_port_init(&port, loop_mac) &&
        !ipv4_init(&ipv4, &port, 0x7F000001u, 0xFF000000u, 0) &&
        !loopback_init(&loopback, &ipv4, 16) &&
        !icmp_init(&icmp, &ipv4, 100, 64) && !udp_init(&udp, &ipv4) &&
        !udp_set_unreachable_callback(&udp, unreachable, &icmp);
    u64 sender = udp_bind(&udp, 0x7F000001u, 10000);
    u64 receiver = udp_bind(&udp, 0, 10001);
    valid = valid && sender && receiver &&
        !udp_bind(&udp, 0x7F000001u, 10000) &&
        !udp_bind(&udp, 0x7F000001u, 10001) &&
        udp_binding_count(&udp) == 2;
    u64 ephemeral = udp_bind(&udp, 0x7F000001u, 0);
    u32 ephemeral_address = 0;
    u16 ephemeral_port = 0;
    valid = valid && ephemeral &&
        !udp_binding_local(&udp, ephemeral,
                           &ephemeral_address, &ephemeral_port) &&
        ephemeral_address == 0x7F000001u &&
        ephemeral_port >= UDP_EPHEMERAL_FIRST &&
        !udp_unbind(&udp, ephemeral) && udp_binding_count(&udp) == 2;
    u8 payload[32];
    for (u32 index = 0; index < sizeof(payload); index++) payload[index] = (u8)index;
    valid = valid && !udp_send(&udp, sender, 0x7F000001u, 10001,
                               payload, sizeof(payload));
    struct udp_binding *receiver_binding =
        &udp.bindings[(u32)receiver - 1];
    valid = valid && receiver_binding->count == 1 &&
        receiver_binding->queue[receiver_binding->head].zero_copy &&
        receiver_binding->queue[receiver_binding->head].packet_pool != 0;
    struct udp_datagram received;
    valid = valid && !udp_receive(&udp, receiver, &received) &&
        received.source_address == 0x7F000001u &&
        received.destination_address == 0x7F000001u &&
        received.source_port == 10000 && received.destination_port == 10001 &&
        received.length == sizeof(payload);
    for (u32 index = 0; index < sizeof(payload); index++)
        if (received.payload[index] != payload[index]) valid = 0;
    static u8 full_payload[UDP_PAYLOAD_MAX];
    for (u32 index = 0; index < UDP_PAYLOAD_MAX; index++)
        full_payload[index] = (u8)index;
    valid = valid && !udp_send(&udp, sender, 0x7F000001u, 10001,
                               full_payload, UDP_PAYLOAD_MAX) &&
        !udp_receive(&udp, receiver, &received) &&
        received.length == UDP_PAYLOAD_MAX &&
        received.payload[0] == 0 &&
        received.payload[UDP_PAYLOAD_MAX - 1] ==
            (u8)(UDP_PAYLOAD_MAX - 1);
    u8 raw[12];
    test_write_be16(raw, 20000);
    test_write_be16(raw + 2, 10001);
    test_write_be16(raw + 4, sizeof(raw));
    raw[6] = 0;
    raw[7] = 0;
    raw[8] = 1;
    raw[9] = 2;
    raw[10] = 3;
    raw[11] = 4;
    struct ipv4_packet_view packet;
    packet.payload = raw;
    packet.packet_pool = 0;
    packet.buffer_id = 0;
    packet.packet_offset = 0;
    packet.header_length = IPV4_HEADER_MIN;
    packet.payload_length = sizeof(raw);
    packet.source = 0x7F000002u;
    packet.destination = 0x7F000001u;
    packet.protocol = 17;
    valid = valid && !udp_ipv4_handler(&ipv4, &packet, &udp) &&
        !udp_receive(&udp, receiver, &received) && received.length == 4;
    u16 checksum = udp_checksum(packet.source, packet.destination,
                                raw, sizeof(raw));
    if (!checksum) checksum = 0xFFFF;
    test_write_be16(raw + 6, checksum);
    raw[8] ^= 1;
    valid = valid && udp_ipv4_handler(&ipv4, &packet, &udp) < 0;
    raw[8] ^= 1;
    test_write_be16(raw + 6, 0);
    test_write_be16(raw + 4, 7);
    valid = valid && udp_ipv4_handler(&ipv4, &packet, &udp) < 0;
    test_write_be16(raw + 4, sizeof(raw));
    test_write_be16(raw, 0);
    valid = valid && udp_ipv4_handler(&ipv4, &packet, &udp) < 0;
    test_write_be16(raw, 20000);
    valid = valid && udp_send(&udp, sender, 0x7F000001u, 19999,
                              payload, sizeof(payload)) < 0 &&
        icmp.stats.port_unreachable_sent == 1 &&
        icmp.stats.destination_unreachable == 1;
    for (u32 index = 0; index < UDP_QUEUE_MAX; index++)
        if (udp_send(&udp, sender, 0x7F000001u, 10001,
                     payload, sizeof(payload)))
            valid = 0;
    valid = valid && udp_send(&udp, sender, 0x7F000001u, 10001,
                              payload, sizeof(payload)) < 0;
    valid = valid && !udp_unbind(&udp, receiver) &&
        udp_receive(&udp, receiver, &received) < 0 &&
        packet_pool_free_count(loopback.pool) == 16;
    receiver = udp_bind(&udp, 0, 10001);
    valid = valid && receiver;
    u32 hot_pages = pmm_free_pages();
    u32 hot_objects = object_active_count();
    u64 start = test_cycles();
    for (u32 index = 0; index < 1024; index++) {
        payload[0] = (u8)index;
        if (udp_send(&udp, sender, 0x7F000001u, 10001,
                     payload, sizeof(payload)) ||
            udp_receive(&udp, receiver, &received))
            valid = 0;
    }
    u64 cycles = test_cycles() - start;
    valid = valid && cycles > 1024 && udp.stats.received >= 1033 &&
        udp.stats.transmitted == 1034 && udp.stats.drops_checksum == 1 &&
        udp.stats.drops_length >= 2 && udp.stats.drops_port == 1 &&
        udp.stats.drops_queue == 1 && packet_pool_free_count(loopback.pool) == 16 &&
        pmm_free_pages() == hot_pages && object_active_count() == hot_objects &&
        !udp_unbind(&udp, sender) && !udp_unbind(&udp, receiver) &&
        udp_unbind(&udp, sender) < 0 && !udp_binding_count(&udp);
    if (valid) {
        serial64_write("Mich x86_64: UDP loopback cycles/datagram=0x");
        serial64_hex(cycles / 1024);
        serial64_write("\n");
    }
    loopback_destroy(&loopback);
    valid = valid && pmm_free_pages() == free_pages &&
        object_active_count() == objects && packet_pool_active_count() == pools;
    return valid ? 0 : -1;
}

int test_route_socket(const struct test64_env *env) {
    u32 free_pages = pmm_free_pages();
    u32 objects = object_active_count();
    u32 sockets = socket_active_count();
    u32 pools = packet_pool_active_count();
    struct route_table routes;
    route_init(&routes);
    int valid = !route_add(&routes, 0, 0, 0xC0A80101u,
                           ROUTE_INTERFACE_VNIC, 100) &&
        !route_add(&routes, 0xC0A80100u, 0xFFFFFF00u, 0,
                   ROUTE_INTERFACE_VNIC, 10) &&
        !route_add(&routes, 0xC0A8012Au, 0xFFFFFFFFu, 0,
                   ROUTE_INTERFACE_VNIC, 1) &&
        !route_add(&routes, 0x7F000000u, 0xFF000000u, 0,
                   ROUTE_INTERFACE_LOOPBACK, 0) &&
        route_add(&routes, 0xC0A80100u, 0xFFFFFF00u, 0,
                  ROUTE_INTERFACE_VNIC, 20) < 0 && route_count(&routes) == 4;
    const struct route_entry *route = route_lookup(&routes, 0xC0A8012Au);
    valid = valid && route && route->prefix_length == 32;
    route = route_lookup(&routes, 0xC0A80144u);
    valid = valid && route && route->prefix_length == 24;
    route = route_lookup(&routes, 0x08080808u);
    valid = valid && route && !route->prefix_length &&
        route->gateway == 0xC0A80101u;
    route = route_lookup(&routes, 0x7F000001u);
    valid = valid && route &&
        route->interface_id == ROUTE_INTERFACE_LOOPBACK &&
        !route_remove(&routes, 0xC0A8012Au, 0xFFFFFFFFu,
                      ROUTE_INTERFACE_VNIC) && route_count(&routes) == 3;
    struct route_table full_routes;
    route_init(&full_routes);
    for (u32 index = 0; index < ROUTE_MAX; index++)
        if (route_add(&full_routes, 0x0A000000u + index, 0xFFFFFFFFu,
                      0, ROUTE_INTERFACE_VNIC, index))
            valid = 0;
    valid = valid && route_count(&full_routes) == ROUTE_MAX &&
        route_add(&full_routes, 0x0B000001u, 0xFFFFFFFFu,
                  0, ROUTE_INTERFACE_VNIC, 0) < 0 &&
        route_lookup(&full_routes, 0x0A00001Fu) != 0;
    const u8 mac[6] = {0x02, 0x4D, 0x49, 0x43, 0x48, 0x60};
    static struct ethernet_port port;
    static struct ipv4_context ipv4;
    static struct loopback_context loopback;
    static struct udp_context udp;
    valid = valid && !ethernet_port_init(&port, mac) &&
        !ipv4_init(&ipv4, &port, 0x7F000001u, 0xFF000000u, 0) &&
        !loopback_init(&loopback, &ipv4, 16) && !udp_init(&udp, &ipv4);
    valid = valid && !socket_init(&udp, &routes);
    struct kernel_object *sender = socket_create();
    struct kernel_object *receiver = socket_create();
    valid = valid && sender && receiver &&
        !socket_bind(sender, 0x7F000001u, 12000) &&
        !socket_bind(receiver, 0x7F000001u, 12001) &&
        socket_bind(receiver, 0x7F000001u, 12002) < 0;
    u8 payload[32];
    for (u32 index = 0; index < sizeof(payload); index++) payload[index] = (u8)index;
    env->target->state = TASK_RUNNING;
    struct kernel_object *receive_event = socket_wait_event(receiver);
    valid = valid && receive_event && event_wait(receive_event, env->target_slot) == 1 &&
        !socket_send_to(sender, 0x7F000001u, 12001,
                        payload, sizeof(payload)) &&
        env->target->state == TASK_RUNNING && *env->target_result == 0;
    struct udp_datagram datagram;
    valid = valid && !socket_receive_from(receiver, &datagram) &&
        datagram.source_port == 12000 && datagram.destination_port == 12001 &&
        datagram.length == sizeof(payload) &&
        socket_send_to(sender, 0xC0A80101u, 12001,
                       payload, sizeof(payload)) < 0;
    for (u32 index = 0; index < sizeof(payload); index++)
        if (datagram.payload[index] != payload[index]) valid = 0;
    u32 hot_pages = pmm_free_pages();
    u32 hot_objects = object_active_count();
    u64 start = test_cycles();
    for (u32 index = 0; index < 1024; index++) {
        payload[0] = (u8)index;
        if (socket_send_to(sender, 0x7F000001u, 12001,
                           payload, sizeof(payload)) ||
            socket_receive_from(receiver, &datagram))
            valid = 0;
    }
    u64 cycles = test_cycles() - start;
    valid = valid && cycles > 1024 && pmm_free_pages() == hot_pages &&
        object_active_count() == hot_objects;
    object_release(sender);
    object_release(receiver);
    valid = valid && !udp_binding_count(&udp) && socket_active_count() == sockets;
    for (u32 index = 0; index < 128 && valid; index++) {
        struct kernel_object *temporary = socket_create();
        if (!temporary || socket_bind(temporary, 0x7F000001u,
                                      (u16)(13000 + (index & 15))))
            valid = 0;
        if (temporary) object_release(temporary);
    }
    struct kernel_object *waiting_socket = socket_create();
    valid = valid && waiting_socket &&
        !socket_bind(waiting_socket, 0x7F000001u, 16000);
    env->target->state = TASK_RUNNING;
    struct kernel_object *waiting_event = socket_wait_event(waiting_socket);
    valid = valid && waiting_event && event_wait(waiting_event, env->target_slot) == 1;
    if (waiting_socket) object_release(waiting_socket);
    valid = valid && env->target->state == TASK_RUNNING &&
        *env->target_result == (u64)(i64)ESRCH &&
        socket_active_count() == sockets;
    if (valid) {
        serial64_write("Mich x86_64: UDP socket cycles/datagram=0x");
        serial64_hex(cycles / 1024);
        serial64_write("\n");
    }
    loopback_destroy(&loopback);
    valid = valid && pmm_free_pages() == free_pages &&
        object_active_count() == objects && socket_active_count() == sockets &&
        packet_pool_active_count() == pools;
    return valid ? 0 : -1;
}


// Driver stand-in: the two test interfaces are cross-wired, frames
// transmitted by one are received by the other, which is how the real
// driver capsule links a VNIC pair.
static u32 socket_file_pump(struct kernel_object *left,
                            struct kernel_object *right,
                            struct driver_domain *owner, u32 now) {
    struct net_packet_descriptor descriptors[NET_INTERFACE_BATCH_MAX];
    u32 moved = 0;
    for (u32 side = 0; side < 2; side++) {
        struct kernel_object *from = side ? right : left;
        struct kernel_object *to = side ? left : right;
        u32 count = net_interface_driver_dequeue_tx_batch(
            from, owner, descriptors, NET_INTERFACE_BATCH_MAX);
        struct net_interface *info = net_interface_get(to);
        for (u32 index = 0; index < count && info; index++) {
            struct net_interface *source = net_interface_get(from);
            const u8 *frame = source ? packet_pool_data(
                source->pool, descriptors[index].buffer_id,
                NET_BUFFER_DRIVER_TX) : 0;
            if (frame)
                net_interface_receive_frame(
                    to, frame + descriptors[index].offset,
                    descriptors[index].length, now);
            net_interface_driver_complete_tx(
                from, owner, descriptors[index].buffer_id);
            moved++;
        }
    }
    return moved;
}

int test_socket_send_file(const struct test64_env *env) {
    u32 free_pages = pmm_free_pages();
    u32 objects = object_active_count();
    u32 sockets = socket_active_count();
    u32 interfaces = net_interface_active_count();
    u32 pools = packet_pool_active_count();
    u32 vnics = vnic_active_count();
    u32 rings = ring_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    struct route_table routes;
    route_init(&routes);
    int valid = !net_interface_init(&routes);
    struct driver_domain owner;
    u8 *owner_bytes = (u8 *)&owner;
    for (usize_t index = 0; index < sizeof(owner); index++) owner_bytes[index] = 0;
    owner.id = 78;
    owner.pid = env->owner->id;
    owner.state = DRIVER_DOMAIN_RUNNING;
    owner.active = 1;
    struct kernel_object *client_vnic = vnic_create(32, 16);
    struct kernel_object *server_vnic = vnic_create(32, 16);
    const u8 client_mac[6] = {0x02, 0x4D, 0x49, 0x43, 0x48, 0x21};
    const u8 server_mac[6] = {0x02, 0x4D, 0x49, 0x43, 0x48, 0x22};
    struct kernel_object *client_interface = client_vnic ? net_interface_create(
        &owner, vnic_pool(client_vnic), vnic_rx_ring(client_vnic),
        vnic_tx_ring(client_vnic), client_mac, 1500, "file0") : 0;
    struct kernel_object *server_interface = server_vnic ?
        net_interface_create(
            &owner, vnic_pool(server_vnic), vnic_rx_ring(server_vnic),
            vnic_tx_ring(server_vnic), server_mac, 1500, "file1") : 0;
    valid = valid && client_vnic && server_vnic && client_interface &&
        server_interface && !net_interface_register(client_interface) &&
        !net_interface_register(server_interface);
    int cip4 = client_interface ? net_interface_set_ipv4(
        client_interface, &owner, 0x0A000002u, 0xFFFFFF00u) : -1;
    int sip4 = server_interface ? net_interface_set_ipv4(
        server_interface, &owner, 0x0A000003u, 0xFFFFFF00u) : -1;
    int clink = client_interface ? net_interface_set_link(
        client_interface, &owner, 1) : -1;
    int slink = server_interface ? net_interface_set_link(
        server_interface, &owner, 1) : -1;
    valid = valid && !cip4 && !sip4 && !clink && !slink;
    struct kernel_object *root = vfs_root();
    struct kernel_object *node = root ? vfs_create(
        root, "payload.bin", VFS_NODE_REGULAR) : 0;
    struct kernel_object *file = node ? vfs_open(node) : 0;
    static u8 payload[5000];
    for (u32 index = 0; index < sizeof(payload); index++)
        payload[index] = (u8)(index * 7 + 3);
    u32 transferred = 0;
    valid = valid && file &&
        !vfs_write(file, 0, payload, sizeof(payload), &transferred) &&
        transferred == sizeof(payload);
    struct kernel_object *listener = socket_create_stream();
    struct kernel_object *client = socket_create_stream();
    valid = valid && listener && client &&
        !socket_stream_listen(listener, server_interface, 8090, 1) &&
        !socket_stream_connect(client, client_interface,
                               0x0A000003u, 8090);
    u32 now = 100;
    for (u32 round = 0; round < 24; round++) {
        u32 delivered = socket_file_pump(
            client_interface, server_interface, &owner, now);
        now += 10;
        if (!delivered) break;
    }
    u32 state = 0;
    u32 readiness = 0;
    i32 error = 0;
    u32 eof = 0;
    valid = valid &&
        !socket_stream_state(client, &state, &readiness, &error, &eof) &&
        state == TCP_STATE_ESTABLISHED;
    struct kernel_object *server = socket_stream_accept(listener);
    valid = valid && server;
    valid = valid &&
        socket_stream_send_file(client, root, 0, 16) < 0 &&
        socket_stream_send_file(client, node, sizeof(payload), 16) < 0 &&
        socket_stream_send_file(client, node, 0, 0) < 0 &&
        !socket_stream_send_file(client, node, 0, sizeof(payload));
    for (u32 round = 0; round < 48; round++) {
        u32 delivered = socket_file_pump(
            client_interface, server_interface, &owner, now);
        now += 10;
        if (!delivered) break;
    }
    static u8 received[8192];
    u32 total = 0;
    for (u32 round = 0; round < 48 && total < sizeof(payload); round++) {
        u32 received_length = 0;
        if (socket_stream_receive(server, received + total,
                                  sizeof(received) - total,
                                  &received_length))
            break;
        total += received_length;
        u32 delivered = socket_file_pump(
            client_interface, server_interface, &owner, now);
        now += 10;
        if (!delivered && !received_length) break;
    }
    valid = valid && total == sizeof(payload);
    for (u32 index = 0; index < total; index++)
        if (received[index] != payload[index]) valid = 0;
    if (server) object_release(server);
    if (listener) object_release(listener);
    if (client) object_release(client);
    if (client_interface) valid = valid && !net_interface_remove(client_interface);
    if (server_interface) valid = valid && !net_interface_remove(server_interface);
    if (client_interface) object_release(client_interface);
    if (server_interface) object_release(server_interface);
    if (client_vnic) object_release(client_vnic);
    if (server_vnic) object_release(server_vnic);
    if (file) object_release(file);
    if (root) valid = valid && !vfs_unlink(root, "payload.bin");
    if (node) object_release(node);
    if (root) object_release(root);
    valid = valid && pmm_free_pages() == free_pages &&
        object_active_count() == objects &&
        socket_active_count() == sockets &&
        net_interface_active_count() == interfaces &&
        packet_pool_active_count() == pools &&
        vnic_active_count() == vnics && ring_active_count() == rings &&
        vfs_node_active_count() == nodes &&
        vfs_file_active_count() == files;
    return valid ? 0 : -1;
}
