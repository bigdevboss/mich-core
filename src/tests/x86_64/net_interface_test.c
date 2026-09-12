#include "net_test.h"

int test_network_revoke(const struct test64_env *env) {
    u32 free_pages = pmm_free_pages();
    u32 objects = object_active_count();
    u32 pools = packet_pool_active_count();
    u32 vnics = vnic_active_count();
    u32 rings = ring_active_count();
    u32 mappings = vm64_object_mapping_count(env->target_space);
    int valid = 1;
    u8 frame[64];
    for (u32 index = 0; index < sizeof(frame); index++) frame[index] = (u8)index;
    for (u32 cycle = 0; cycle < 16 && valid; cycle++) {
        struct kernel_object *vnic = vnic_create(16, 8);
        struct kernel_object *pool = vnic_pool(vnic);
        struct kernel_object *rx = vnic_rx_ring(vnic);
        struct kernel_object *tx = vnic_tx_ring(vnic);
        struct kernel_object *pool_memory = packet_pool_backing(pool);
        struct kernel_object *rx_memory = ring_resource_backing(rx);
        struct kernel_object *tx_memory = ring_resource_backing(tx);
        if (!vnic || !pool || !rx || !tx || !pool_memory || !rx_memory ||
            !tx_memory || vm64_map_page_object(env->target_space,
                                               VM64_DRIVER_BASE,
                                               pool_memory, 1) ||
            vm64_map_page_object(env->target_space,
                                 VM64_DRIVER_BASE + 0x20000, rx_memory, 1) ||
            vm64_map_page_object(env->target_space,
                                 VM64_DRIVER_BASE + 0x21000, tx_memory, 1)) {
            if (vnic) object_release(vnic);
            valid = 0;
            break;
        }
        for (u32 index = 0; index < 8; index++)
            if (vnic_inject(vnic, frame, sizeof(frame))) valid = 0;
        u64 tx_ids[8];
        for (u32 index = 0; index < 8; index++) {
            tx_ids[index] = vnic_acquire_tx(vnic);
            if (!tx_ids[index] || vnic_submit_tx(
                    vnic, tx_ids[index], NET_PACKET_HEADROOM,
                    sizeof(frame), 0))
                valid = 0;
        }
        valid = valid && !packet_pool_free_count(pool) &&
            vnic_inject(vnic, frame, sizeof(frame)) < 0 &&
            !vnic_acquire_tx(vnic) && !vnic_revoke(vnic) &&
            vm64_object_mapping_count(env->target_space) == mappings &&
            !packet_pool_data(pool, tx_ids[0], NET_BUFFER_TX_QUEUED) &&
            ring_resource_submit(rx, 1) < 0 && ring_resource_submit(tx, 1) < 0;
        object_release(vnic);
        valid = valid && pmm_free_pages() == free_pages &&
            object_active_count() == objects &&
            packet_pool_active_count() == pools && vnic_active_count() == vnics &&
            ring_active_count() == rings;
    }
    return valid ? 0 : -1;
}

__attribute__((cold, noinline, optimize("Os"))) int test_net_interface(
    const struct test64_env *env) {
    u32 free_pages = pmm_free_pages();
    u32 objects = object_active_count();
    u32 interfaces = net_interface_active_count();
    u32 pools = packet_pool_active_count();
    u32 vnics = vnic_active_count();
    u32 rings = ring_active_count();
    struct route_table routes;
    route_init(&routes);
    if (net_interface_init(&routes)) return -1;
    struct driver_domain owner;
    u8 *owner_bytes = (u8 *)&owner;
    for (usize_t index = 0; index < sizeof(owner); index++) owner_bytes[index] = 0;
    owner.id = 77;
    owner.pid = env->owner->id;
    owner.state = DRIVER_DOMAIN_RUNNING;
    owner.active = 1;
    struct kernel_object *vnic = vnic_create(16, 8);
    struct kernel_object *duplicate_vnic = vnic_create(8, 8);
    const u8 mac[6] = {0x02, 0x4D, 0x49, 0x43, 0x48, 0x10};
    struct kernel_object *interface = vnic ? net_interface_create(
        &owner, vnic_pool(vnic), vnic_rx_ring(vnic), vnic_tx_ring(vnic),
        mac, 1500, "eth0") : 0;
    struct kernel_object *duplicate = duplicate_vnic ? net_interface_create(
        &owner, vnic_pool(duplicate_vnic), vnic_rx_ring(duplicate_vnic),
        vnic_tx_ring(duplicate_vnic), mac, 1500, "eth0") : 0;
    int valid = vnic && duplicate_vnic && interface && duplicate &&
        !net_interface_register(interface) &&
        net_interface_register(duplicate) < 0 &&
        net_interface_init(&routes) < 0;
    struct net_interface *info = net_interface_get(interface);
    valid = valid && info && info->interface_id && info->generation == 1 &&
        info->state == NET_INTERFACE_DOWN &&
        !route_add_generation(&routes, 0x0A000000u, 0xFFFFFF00u, 0,
                              info->interface_id, info->generation, 10) &&
        route_count(&routes) == 1 &&
        net_interface_lookup(info->interface_id, info->generation) == interface;
    struct kernel_object *link_event = net_interface_wait_event(interface);
    env->target->state = TASK_RUNNING;
    valid = valid && link_event && event_wait(link_event, env->target_slot) == 0 &&
        event_wait(link_event, env->target_slot) == 1 &&
        !net_interface_set_ipv4(interface, &owner,
                                0x0A000002u, 0xFFFFFF00u) &&
        env->target->state == TASK_RUNNING && *env->target_result == 0 &&
        !net_interface_set_link(interface, &owner, 1) &&
        info->state == NET_INTERFACE_UP;
    // Net-interface RX path benchmark: measures the per-packet cost of the real
    // RX path (ethernet -> protocol), including the per-frame ARP cache sweep.
    // `now` advances 1 ms per frame so the throttled sweep runs once per 1000
    // frames (its steady-state amortized cost is what we measure).
    {
        const u32 bench_rounds = 1024;
        u8 bench_frame[64];
        for (u32 b = 0; b < sizeof(bench_frame); b++) bench_frame[b] = (u8)b;
        build_ethernet_frame(bench_frame, mac, mac, 0x0800, sizeof(bench_frame));
        ipv4_build_header(bench_frame + 14, 20, 256,
                          0x0A000002u, 0x0A000002u, 17, 64, 1, 1);
        u64 bench_start = test_cycles();
        for (u32 round = 0; round < bench_rounds; round++)
            net_interface_receive_frame(interface, bench_frame,
                sizeof(bench_frame), 1 + round);
        u64 bench_cycles = test_cycles() - bench_start;
        valid = valid && bench_cycles > bench_rounds;
        if (valid) {
            serial64_write("Mich x86_64: net-interface RX cycles/packet=0x");
            serial64_hex(bench_cycles / bench_rounds);
            serial64_write("\n");
        }
    }
    u64 packet_id = net_interface_acquire_tx(interface);
    u8 *packet = (u8 *)net_interface_packet_data(interface, packet_id);
    u32 packet_offset = NET_PACKET_HEADROOM;
    if (packet)
        ipv4_build_header(packet + packet_offset, 20, 16,
                          0x0A000002u, 0x0A000005u, 17, 64, 1, 1);
    valid = valid && packet_id && packet &&
        net_interface_route_send(&routes, packet_id, packet_offset, 36,
                                 0x0A000005u, 10) == 1 &&
        info->pending_count == 1;
    struct net_packet_descriptor arp_request;
    valid = valid && !vnic_drain_tx(vnic, &arp_request) &&
        arp_request.length == 42;
    const u8 peer_mac[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0x05};
    u8 arp_reply[64];
    build_ethernet_frame(arp_reply, mac, peer_mac, 0x0806,
                         sizeof(arp_reply));
    build_arp_payload(arp_reply + 14, 2, peer_mac, 0x0A000005u,
                      mac, 0x0A000002u);
    valid = valid && !net_interface_receive_frame(
        interface, arp_reply, 42, 11) && !info->pending_count;
    struct net_packet_descriptor ipv4_frame;
    valid = valid && !vnic_drain_tx(vnic, &ipv4_frame) &&
        ipv4_frame.length == 50 && info->stats.arp_flushed == 1;
    struct page_resource *pool_memory = page_resource_get(
        packet_pool_backing(info->pool));
    u32 ipv4_slot = (u32)ipv4_frame.buffer_id - 1;
    u8 *sent = pool_memory && ipv4_slot < pool_memory->pages ?
        (u8 *)(uptr_t)pool_memory->physical[ipv4_slot] + ipv4_frame.offset : 0;
    valid = valid && sent && sent[0] == peer_mac[0] && sent[5] == peer_mac[5] &&
        sent[12] == 0x08 && sent[13] == 0x00;
    u64 oversized = net_interface_acquire_tx(interface);
    valid = valid && oversized && net_interface_send_ipv4(
        interface, oversized, NET_PACKET_HEADROOM, 1501,
        0x0A000005u, 0x0A000005u, 12) < 0 &&
        info->stats.mtu_drops == 1 &&
        !packet_pool_release(info->pool, oversized, NET_BUFFER_TX);
    u64 timeout_packet = net_interface_acquire_tx(interface);
    u8 *timeout_data = (u8 *)net_interface_packet_data(interface, timeout_packet);
    if (timeout_data)
        ipv4_build_header(timeout_data + NET_PACKET_HEADROOM, 20, 0,
                          0x0A000002u, 0x0A000006u, 1, 64, 2, 1);
    valid = valid && timeout_packet && timeout_data &&
        net_interface_route_send(&routes, timeout_packet,
                                 NET_PACKET_HEADROOM, 20,
                                 0x0A000006u, 20) == 1 &&
        !vnic_drain_tx(vnic, &arp_request);
    net_interface_tick(interface, 321);
    valid = valid && !info->pending_count && info->stats.arp_timeouts == 1 &&
        packet_pool_free_count(info->pool) == 16;
    u64 driver_rx = net_interface_driver_acquire_rx(interface, &owner);
    u8 *driver_rx_data = (u8 *)packet_pool_data(
        info->pool, driver_rx, NET_BUFFER_DRIVER_RX);
    if (driver_rx_data)
        for (u32 index = 0; index < 42; index++)
            driver_rx_data[NET_PACKET_HEADROOM + index] = arp_reply[index];
    valid = valid && driver_rx && driver_rx_data &&
        !net_interface_driver_receive(interface, &owner, driver_rx,
                                      NET_PACKET_HEADROOM, 42, 400) &&
        net_interface_driver_release_rx(interface, &owner, driver_rx) < 0;
    u64 driver_tx = vnic_acquire_tx(vnic);
    valid = valid && driver_tx &&
        !vnic_submit_tx(vnic, driver_tx, NET_PACKET_HEADROOM, 64, 0);
    struct net_packet_descriptor driver_tx_descriptor;
    valid = valid && !net_interface_driver_dequeue_tx(
        interface, &owner, &driver_tx_descriptor) &&
        driver_tx_descriptor.buffer_id == driver_tx &&
        packet_pool_data(info->pool, driver_tx, NET_BUFFER_DRIVER_TX) &&
        !net_interface_driver_complete_tx(interface, &owner, driver_tx) &&
        net_interface_driver_complete_tx(interface, &owner, driver_tx) < 0 &&
        packet_pool_free_count(info->pool) == 16;
    u64 batch_rx[3];
    u32 batch_acquired = net_interface_driver_acquire_rx_batch(
        interface, &owner, 3, batch_rx);
    valid = valid && batch_acquired == 3 && batch_rx[0] &&
        batch_rx[0] != batch_rx[1] && batch_rx[1] != batch_rx[2] &&
        packet_pool_free_count(info->pool) == 13;
    struct net_interface_buffer_request batch_requests[3];
    for (u32 index = 0; index < 3; index++) {
        u8 *frame = (u8 *)packet_pool_data(
            info->pool, batch_rx[index], NET_BUFFER_DRIVER_RX);
        if (frame)
            for (u32 byte = 0; byte < 42; byte++)
                frame[NET_PACKET_HEADROOM + byte] = arp_reply[byte];
        batch_requests[index].buffer_id = batch_rx[index];
        batch_requests[index].offset = NET_PACKET_HEADROOM;
        batch_requests[index].length = 42;
    }
    u32 batch_processed = net_interface_driver_receive_batch(
        interface, &owner, batch_requests, 3, 450);
    valid = valid && batch_processed == 3 &&
        packet_pool_free_count(info->pool) == 16;
    batch_acquired = net_interface_driver_acquire_rx_batch(
        interface, &owner, 2, batch_rx);
    valid = valid && batch_acquired == 2 &&
        packet_pool_free_count(info->pool) == 14;
    {
        u8 *frame = (u8 *)packet_pool_data(
            info->pool, batch_rx[0], NET_BUFFER_DRIVER_RX);
        if (frame)
            for (u32 byte = 0; byte < 42; byte++)
                frame[NET_PACKET_HEADROOM + byte] = arp_reply[byte];
        batch_requests[0].buffer_id = batch_rx[0];
        batch_requests[0].offset = NET_PACKET_HEADROOM;
        batch_requests[0].length = 42;
        batch_requests[1].buffer_id = batch_rx[1];
        batch_requests[1].offset = NET_PACKET_HEADROOM;
        batch_requests[1].length = 0;
    }
    u32 batch_stopped = net_interface_driver_receive_batch(
        interface, &owner, batch_requests, 2, 500);
    valid = valid && batch_stopped == 1 &&
        !packet_pool_release(info->pool, batch_rx[1], NET_BUFFER_DRIVER_RX) &&
        packet_pool_free_count(info->pool) == 16;
    valid = valid &&
        net_interface_driver_acquire_rx_batch(interface, &owner, 9, batch_rx)
            == 0 &&
        net_interface_driver_receive_batch(
            interface, &owner, batch_requests, 0, 500) == 0;
    u64 batch_tx1 = vnic_acquire_tx(vnic);
    u64 batch_tx2 = vnic_acquire_tx(vnic);
    valid = valid && batch_tx1 && batch_tx2 &&
        !vnic_submit_tx(vnic, batch_tx1, NET_PACKET_HEADROOM, 64, 0) &&
        !vnic_submit_tx(vnic, batch_tx2, NET_PACKET_HEADROOM, 64, 0);
    struct net_packet_descriptor batch_descriptors[2];
    u32 batch_dequeued = net_interface_driver_dequeue_tx_batch(
        interface, &owner, batch_descriptors, 2);
    valid = valid && batch_dequeued == 2 &&
        batch_descriptors[0].buffer_id == batch_tx1 &&
        batch_descriptors[1].buffer_id == batch_tx2;
    u64 batch_ids[2] = {batch_tx1, batch_tx2};
    u32 batch_completed = net_interface_driver_complete_tx_batch(
        interface, &owner, batch_ids, 2);
    valid = valid && batch_completed == 2 &&
        packet_pool_free_count(info->pool) == 16;
    valid = valid &&
        net_interface_driver_complete_tx_batch(
            interface, &owner, batch_ids, 1) == 0;
    u32 owner_handle = handle_open(env->owner, interface,
                                   KRIGHT_READ | KRIGHT_WAIT | KRIGHT_TRANSFER);
    u32 consumer_handle = owner_handle ?
        handle_duplicate(env->owner, env->target, owner_handle,
                         KRIGHT_READ | KRIGHT_WAIT) : 0;
    struct kernel_object *interface_event = net_interface_wait_event(interface);
    env->target->state = TASK_RUNNING;
    valid = valid && owner_handle && consumer_handle && interface_event &&
        !event_reset(interface_event) &&
        event_wait(interface_event, env->target_slot) == 1 &&
        env->target->state == TASK_BLOCKED_EVENT;
    u32 old_generation = info->generation;
    u32 interface_id = info->interface_id;
    net_interface_task_died(owner.pid);
    owner.pid = env->target->id;
    struct net_interface *replacement = net_interface_get(duplicate);
    valid = valid && info->state == NET_INTERFACE_REMOVED &&
        info->generation != old_generation &&
        !net_interface_lookup(interface_id, old_generation) &&
        !route_count(&routes) && ring_resource_get(info->rx_ring)->revoked &&
        ring_resource_get(info->tx_ring)->revoked &&
        !packet_pool_free_count(info->pool) &&
        !handle_get(env->owner, owner_handle, KRIGHT_READ,
                    KOBJECT_NET_INTERFACE) &&
        !handle_get(env->target, consumer_handle, KRIGHT_READ,
                    KOBJECT_NET_INTERFACE) &&
        env->target->state == TASK_RUNNING && *env->target_result == 0 &&
        net_interface_remove(interface) < 0 &&
        !net_interface_register(duplicate) && replacement &&
        replacement->interface_id != interface_id &&
        replacement->generation == 1 &&
        replacement->state == NET_INTERFACE_DOWN &&
        net_interface_lookup(replacement->interface_id,
                             replacement->generation) == duplicate &&
        !net_interface_set_link(duplicate, &owner, 1) &&
        replacement->state == NET_INTERFACE_UP;
    object_release(interface);
    if (duplicate && duplicate->active && replacement && replacement->registered)
        valid = valid && !net_interface_remove(duplicate);
    object_release(duplicate);
    object_release(vnic);
    object_release(duplicate_vnic);
    valid = valid && pmm_free_pages() == free_pages &&
        object_active_count() == objects &&
        net_interface_active_count() == interfaces &&
        packet_pool_active_count() == pools && vnic_active_count() == vnics &&
        ring_active_count() == rings;
    return valid ? 0 : -1;
}

int test_network_fuzz(void) {
    static u8 data[1600];
    u32 seed = 0x4D494348u;
    for (u32 index = 0; index < sizeof(data); index++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        data[index] = (u8)seed;
    }
    for (u32 iteration = 0; iteration < 4096; iteration++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        u32 offset = seed % sizeof(data);
        data[offset] ^= (u8)(seed >> 8);
        u32 length = (seed >> 16) % (sizeof(data) + 1);
        struct ethernet_frame_view ethernet;
        int ethernet_result = ethernet_parse(data, length, &ethernet);
        if (!ethernet_result &&
            (ethernet.frame != data || ethernet.payload < data ||
             ethernet.payload > data + length ||
             ethernet.payload_length > length))
            return -1;
        struct ipv4_packet_view ipv4;
        int ipv4_result = ipv4_parse(data, length, &ipv4);
        if (!ipv4_result &&
            (ipv4.packet != data || ipv4.payload < data ||
             ipv4.payload > data + length ||
             ipv4.packet_length > length || ipv4.payload_length > length))
            return -1;
        if (length >= UDP_HEADER_SIZE)
            (void)udp_checksum(0x0A000001u, 0x0A000002u, data, length);
        if (length >= ICMP_HEADER_SIZE)
            (void)icmp_checksum(data, length);
    }
    return 0;
}
