#include "net_test.h"

int test_net_foundation(void) {
    u32 free_pages = pmm_free_pages();
    u32 objects = object_active_count();
    u32 pools = packet_pool_active_count();
    u32 vnics = vnic_active_count();
    u32 rings = ring_active_count();
    struct kernel_object *vnic = vnic_create(16, 8);
    struct kernel_object *pool = vnic_pool(vnic);
    struct kernel_object *rx = vnic_rx_ring(vnic);
    struct kernel_object *tx = vnic_tx_ring(vnic);
    if (!vnic || !pool || !rx || !tx) {
        if (vnic) object_release(vnic);
        return -1;
    }
    u8 frame[64];
    for (u32 index = 0; index < sizeof(frame); index++) frame[index] = (u8)index;
    int valid = 1;
    for (u32 index = 0; index < 8; index++)
        if (vnic_inject(vnic, frame, sizeof(frame))) valid = 0;
    valid = valid && vnic_inject(vnic, frame, sizeof(frame)) < 0;
    for (u32 index = 0; index < 8 && valid; index++) {
        struct net_packet_descriptor descriptor;
        if (vnic_receive(vnic, &descriptor)) {
            valid = 0;
            break;
        }
        u8 *data = (u8 *)packet_pool_data(
            pool, descriptor.buffer_id, NET_BUFFER_STACK);
        if (!data || descriptor.offset != NET_PACKET_HEADROOM ||
            descriptor.length != sizeof(frame)) {
            valid = 0;
            break;
        }
        for (u32 byte = 0; byte < sizeof(frame); byte++)
            if (data[descriptor.offset + byte] != frame[byte]) valid = 0;
        if (vnic_release_rx(vnic, descriptor.buffer_id)) valid = 0;
    }
    valid = valid && !vnic_inject(vnic, frame, sizeof(frame));
    struct ring_resource *rx_state = ring_resource_get(rx);
    struct net_packet_descriptor *forged = rx_state ?
        ring_resource_descriptor(rx, rx_state->consumer) : 0;
    if (forged) forged->reserved0 = 1;
    struct net_packet_descriptor rejected;
    valid = valid && forged && vnic_receive(vnic, &rejected) < 0 &&
            packet_pool_free_count(pool) == 16;
    u64 tx_id = vnic_acquire_tx(vnic);
    u8 *tx_data = (u8 *)packet_pool_data(pool, tx_id, NET_BUFFER_TX);
    if (tx_data)
        for (u32 index = 0; index < sizeof(frame); index++)
            tx_data[NET_PACKET_HEADROOM + index] = frame[index];
    struct net_packet_descriptor tx_descriptor;
    valid = valid && tx_id && tx_data &&
        !vnic_submit_tx(vnic, tx_id, NET_PACKET_HEADROOM,
                        sizeof(frame), 0) &&
        !vnic_drain_tx(vnic, &tx_descriptor) &&
        tx_descriptor.buffer_id == tx_id &&
        packet_pool_release(pool, tx_id, NET_BUFFER_TX_QUEUED) < 0;
    u32 hot_pages = pmm_free_pages();
    u32 hot_objects = object_active_count();
    struct vnic_benchmark_request benchmark;
    benchmark.packets = 1024;
    benchmark.batch_size = 8;
    benchmark.payload_size = sizeof(frame);
    benchmark.completed = 0;
    benchmark.cycles = 0;
    struct vnic_stats stats;
    valid = valid && !vnic_benchmark_run(vnic, &benchmark) &&
        benchmark.completed == 1024 && benchmark.cycles > benchmark.completed &&
        pmm_free_pages() == hot_pages && object_active_count() == hot_objects &&
        !vnic_get_stats(vnic, &stats) && stats.rx_packets == 1033 &&
        stats.rx_drops == 2 && stats.tx_packets == 1 &&
        packet_pool_free_count(pool) == 16 && !vnic_revoke(vnic);
    if (valid) {
        serial64_write("Mich x86_64: virtual NIC cycles/packet=0x");
        serial64_hex(benchmark.cycles / benchmark.completed);
        serial64_write("\n");
    }
    object_release(vnic);
    valid = valid && pmm_free_pages() == free_pages &&
        object_active_count() == objects &&
        packet_pool_active_count() == pools && vnic_active_count() == vnics &&
        ring_active_count() == rings;
    return valid ? 0 : -1;
}

struct ethernet_test_context {
    u32 calls;
    u32 bytes;
    u32 vlan_calls;
};

static int ethernet_test_handler(struct ethernet_port *port,
                                 const struct ethernet_frame_view *frame,
                                 void *context) {
    (void)port;
    struct ethernet_test_context *test =
        (struct ethernet_test_context *)context;
    if (!test || !frame || !frame->payload_length) return -1;
    test->calls++;
    test->bytes += frame->payload_length;
    if (frame->flags & ETHERNET_FLAG_VLAN) test->vlan_calls++;
    return 0;
}

int test_ethernet(void) {
    u32 free_pages = pmm_free_pages();
    u32 objects = object_active_count();
    u32 pools = packet_pool_active_count();
    u32 vnics = vnic_active_count();
    u32 rings = ring_active_count();
    struct kernel_object *vnic = vnic_create(32, 16);
    if (!vnic) return -1;
    const u8 local[6] = {0x02, 0x4D, 0x49, 0x43, 0x48, 0x01};
    const u8 source[6] = {0x02, 0x10, 0x20, 0x30, 0x40, 0x50};
    const u8 foreign[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    const u8 broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const u8 multicast[6] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};
    struct ethernet_port port;
    struct ethernet_test_context ipv4 = {0, 0, 0};
    struct ethernet_test_context arp = {0, 0, 0};
    int valid = !ethernet_port_init(&port, local) &&
        !ethernet_register_handler(&port, 0x0800,
                                   ethernet_test_handler, &ipv4) &&
        !ethernet_register_handler(&port, 0x0806,
                                   ethernet_test_handler, &arp) &&
        ethernet_register_handler(&port, 0x0800,
                                  ethernet_test_handler, &ipv4) < 0;
    u8 frame[128];
    build_ethernet_frame(frame, local, source, 0x0800, 64);
    valid = valid && !ethernet_receive(&port, frame, 64);
    build_ethernet_frame(frame, foreign, source, 0x0800, 64);
    valid = valid && ethernet_receive(&port, frame, 64) < 0;
    build_ethernet_frame(frame, broadcast, source, 0x0806, 64);
    valid = valid && !ethernet_receive(&port, frame, 64);
    build_ethernet_frame(frame, multicast, source, 0x0800, 64);
    valid = valid && ethernet_receive(&port, frame, 64) < 0 &&
        !ethernet_port_set_filter(&port, 0, 1) &&
        !ethernet_receive(&port, frame, 64);
    build_ethernet_frame(frame, local, source, 0x0800, 13);
    valid = valid && ethernet_receive(&port, frame, 13) < 0;
    u8 bad_source[6] = {0x01, 0, 0, 0, 0, 1};
    build_ethernet_frame(frame, local, bad_source, 0x0800, 64);
    valid = valid && ethernet_receive(&port, frame, 64) < 0;
    build_ethernet_frame(frame, local, source, 0x8100, 68);
    frame[14] = 0x01;
    frame[15] = 0x23;
    frame[16] = 0x08;
    frame[17] = 0x00;
    valid = valid && !ethernet_receive(&port, frame, 68);
    build_ethernet_frame(frame, local, source, 0x8100, 17);
    valid = valid && ethernet_receive(&port, frame, 17) < 0;
    build_ethernet_frame(frame, local, source, 0x8100, 68);
    frame[14] = 0;
    frame[15] = 1;
    frame[16] = 0x81;
    frame[17] = 0x00;
    valid = valid && ethernet_receive(&port, frame, 68) < 0;
    build_ethernet_frame(frame, local, source, 0x1234, 64);
    valid = valid && ethernet_receive(&port, frame, 64) < 0;
    build_ethernet_frame(frame, local, source, 100, 64);
    valid = valid && ethernet_receive(&port, frame, 64) < 0;
    build_ethernet_frame(frame, local, source, 0x0800, 64);
    for (u32 index = 0; index < 16; index++)
        if (vnic_inject(vnic, frame, 64)) valid = 0;
    valid = valid && ethernet_receive_vnic_batch(&port, vnic, 16) == 16;
    u32 hot_pages = pmm_free_pages();
    u32 hot_objects = object_active_count();
    u64 start = test_cycles();
    u32 completed = 0;
    for (u32 round = 0; round < 64 && valid; round++) {
        for (u32 index = 0; index < 16; index++)
            if (vnic_inject(vnic, frame, 64)) valid = 0;
        completed += ethernet_receive_vnic_batch(&port, vnic, 16);
    }
    u64 cycles = test_cycles() - start;
    valid = valid && completed == 1024 && ipv4.calls == 1043 &&
        arp.calls == 1 && ipv4.vlan_calls == 1 &&
        port.stats.drops_length >= 2 && port.stats.drops_source == 1 &&
        port.stats.drops_filter == 2 && port.stats.drops_protocol >= 2 &&
        pmm_free_pages() == hot_pages && object_active_count() == hot_objects &&
        packet_pool_free_count(vnic_pool(vnic)) == 32 && cycles > completed;
    if (valid) {
        serial64_write("Mich x86_64: Ethernet cycles/frame=0x");
        serial64_hex(cycles / completed);
        serial64_write("\n");
    }
    vnic_revoke(vnic);
    object_release(vnic);
    valid = valid && pmm_free_pages() == free_pages &&
        object_active_count() == objects &&
        packet_pool_active_count() == pools && vnic_active_count() == vnics &&
        ring_active_count() == rings;
    return valid ? 0 : -1;
}
