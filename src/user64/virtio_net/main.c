#include <mich/syscall.h>
#include <mich/event.h>
#include <mich/driver.h>
#include <mich/virtio.h>
#include <mich/net.h>
#include <mich/net_interface.h>
#include <mich/hardware.h>
#include <mich/bridge.h>
#include <mich/timer.h>
#include <mich/wait.h>
#include <mich/vfs.h>
#include <mich/firmware.h>
#include <checksum.h>

#include "capsule.h"

static unsigned long long read_cycles(void) {
    unsigned int low;
    unsigned int high;
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) :: "memory");
    return ((unsigned long long)high << 32) | low;
}

static unsigned int dhcp_transaction_id(
    const struct virtio_net_capsule *capsule) {
    unsigned long long cycles = read_cycles();
    unsigned int value = (unsigned int)(cycles ^ (cycles >> 32));
    for (unsigned int index = 0; index < 6; index++)
        value = (value << 5) ^ (value >> 2) ^ capsule->mac[index];
    unsigned int eax;
    unsigned int ebx;
    unsigned int ecx;
    unsigned int edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));
    if (ecx & (1u << 30)) {
        unsigned int random;
        unsigned char valid;
        __asm__ volatile("rdrand %0; setc %1"
                         : "=r"(random), "=qm"(valid));
        if (valid) value ^= random;
    }
    (void)eax;
    (void)ebx;
    (void)edx;
    return value ? value : 1;
}

static void write_cycle_metric(const char *prefix,
                               unsigned long long cycles) {
    char text[96];
    unsigned int length = 0;
    while (prefix[length] && length < sizeof(text) - 19) {
        text[length] = prefix[length];
        length++;
    }
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned int digit = (unsigned int)(cycles >> shift) & 15u;
        text[length++] = digit < 10 ? (char)('0' + digit) :
                                     (char)('A' + digit - 10);
    }
    text[length++] = '\n';
    text[length] = 0;
    mich_write(text);
}

static int transition(struct virtio_net_capsule *capsule,
                      unsigned int expected, unsigned int next) {
    if (!capsule || capsule->state != expected || next != expected + 1)
        return -1;
    capsule->state = next;
    return 0;
}

static int bootstrap(struct virtio_net_capsule *capsule) {
    struct mich_driver_bootstrap_info info;
    if (mich_driver_bootstrap(&info) ||
        info.abi_version != MICH_DRIVER_ABI_VERSION ||
        info.size != sizeof(info) || info.vendor_id != 0x1AF4 ||
        (info.device_id != 0x1000 && info.device_id != 0x1041))
        return -1;
    for (unsigned int index = 0; index < info.resource_count; index++) {
        struct mich_driver_resource_info *resource = &info.resources[index];
        if (resource->kind == MICH_DRIVER_RESOURCE_PCI && !resource->index)
            capsule->pci_handle = resource->handle;
        if (resource->kind == MICH_DRIVER_RESOURCE_MSIX_IRQ &&
            resource->index == 0)
            capsule->config_irq_handle = resource->handle;
        if (resource->kind == MICH_DRIVER_RESOURCE_MSIX_IRQ &&
            resource->index == 1)
            capsule->rx_irq_handle = resource->handle;
        if (resource->kind == MICH_DRIVER_RESOURCE_MSIX_IRQ &&
            resource->index == 2)
            capsule->tx_irq_handle = resource->handle;
        if (resource->kind == MICH_DRIVER_RESOURCE_BRIDGE)
            capsule->bridge_handle = resource->handle;
    }
    if (!capsule->pci_handle || !capsule->bridge_handle ||
        !capsule->rx_irq_handle || !capsule->tx_irq_handle)
        return -1;
    capsule->restart_count = info.restart_count;
    capsule->device_handle = mich_virtio_open(capsule->pci_handle);
    if ((int)capsule->device_handle <= 0) return -1;
    return transition(capsule, VIRTIO_NET_STATE_CREATED,
                      VIRTIO_NET_STATE_BOOTSTRAPPED);
}

static int negotiate(struct virtio_net_capsule *capsule) {
    struct mich_virtio_feature_request request;
    request.wanted = MICH_VIRTIO_FEATURE_VERSION_1 |
                     MICH_VIRTIO_NET_FEATURE_MAC |
                     MICH_VIRTIO_NET_FEATURE_STATUS;
    request.required = MICH_VIRTIO_FEATURE_VERSION_1 |
                       MICH_VIRTIO_NET_FEATURE_MAC;
    request.device_features = 0;
    request.driver_features = 0;
    if (mich_virtio_negotiate(capsule->device_handle, &request) ||
        (request.driver_features & request.required) != request.required)
        return -1;
    capsule->negotiated_features = request.driver_features;
    return transition(capsule, VIRTIO_NET_STATE_BOOTSTRAPPED,
                      VIRTIO_NET_STATE_FEATURES);
}

static int read_config(struct virtio_net_capsule *capsule) {
    struct mich_virtio_config_request request;
    request.offset = 0;
    request.length = capsule->negotiated_features &
                     MICH_VIRTIO_NET_FEATURE_STATUS ? 8 : 6;
    request.generation = 0;
    request.reserved = 0;
    for (unsigned int index = 0; index < MICH_VIRTIO_CONFIG_DATA_MAX; index++)
        request.data[index] = 0;
    if (mich_virtio_read_config(capsule->device_handle, &request)) return -1;
    unsigned int nonzero = 0;
    for (unsigned int index = 0; index < 6; index++) {
        capsule->mac[index] = request.data[index];
        nonzero |= request.data[index];
    }
    if (!nonzero) return -1;
    capsule->link_status = capsule->negotiated_features &
        MICH_VIRTIO_NET_FEATURE_STATUS ?
        (unsigned short)request.data[6] |
        ((unsigned short)request.data[7] << 8) : 1;
    capsule->config_generation = request.generation;
    return transition(capsule, VIRTIO_NET_STATE_FEATURES,
                      VIRTIO_NET_STATE_CONFIG);
}

static int create_queue(struct virtio_net_capsule *capsule,
                        unsigned short queue_index,
                        unsigned long long address,
                        unsigned int *handle, unsigned int *size) {
    struct mich_virtqueue_create_request request;
    request.device_handle = capsule->device_handle;
    request.queue_index = queue_index;
    request.queue_size = 256;
    request.queue_handle = 0;
    request.descriptor_offset = 0;
    request.available_offset = 0;
    request.used_offset = 0;
    request.total_bytes = 0;
    if (mich_virtqueue_create(&request)) return -1;
    if (!request.queue_handle || !request.queue_size ||
        request.queue_size > 256 ||
        (request.queue_size & (request.queue_size - 1)) ||
        (request.descriptor_offset & 15) || (request.available_offset & 1) ||
        (request.used_offset & 3) ||
        request.descriptor_offset >= request.available_offset ||
        request.available_offset >= request.used_offset ||
        request.total_bytes <= request.used_offset)
        return -2;
    if (mich_virtqueue_map(request.queue_handle, address)) return -3;
    *handle = request.queue_handle;
    *size = request.queue_size;
    return 0;
}

static int create_queues(struct virtio_net_capsule *capsule) {
    int result = create_queue(capsule, 0, VIRTIO_NET_RX_ADDRESS,
                              &capsule->rx_handle, &capsule->rx_size);
    if (result) return result;
    mich_write("Mich virtio-net: RX queue created\n");
    result = create_queue(capsule, 1, VIRTIO_NET_TX_ADDRESS,
                          &capsule->tx_handle, &capsule->tx_size);
    if (result) return result - 3;
    return transition(capsule, VIRTIO_NET_STATE_CONFIG,
                      VIRTIO_NET_STATE_QUEUES);
}

static int setup_interrupts(struct virtio_net_capsule *capsule) {
    if (mich_virtqueue_set_msix(capsule->rx_handle,
                                capsule->rx_irq_handle) ||
        mich_virtqueue_set_msix(capsule->tx_handle,
                                capsule->tx_irq_handle) ||
        mich_irq_bind(capsule->rx_irq_handle, capsule->bridge_handle) ||
        mich_irq_bind(capsule->tx_irq_handle, capsule->bridge_handle) ||
        mich_irq_set_mask(capsule->rx_irq_handle, 0) ||
        mich_irq_set_mask(capsule->tx_irq_handle, 0))
        return -1;
    return 0;
}

static int setup_interface(struct virtio_net_capsule *capsule) {
    struct mich_vnic_create_request vnic;
    vnic.buffer_count = 32;
    vnic.ring_capacity = 32;
    vnic.vnic_handle = 0;
    vnic.pool_handle = 0;
    vnic.rx_ring_handle = 0;
    vnic.tx_ring_handle = 0;
    if (mich_vnic_create(&vnic) ||
        mich_packet_pool_map(vnic.pool_handle, VIRTIO_NET_POOL_ADDRESS))
        return -1;
    capsule->vnic_handle = vnic.vnic_handle;
    capsule->pool_handle = vnic.pool_handle;
    struct mich_net_interface_create_request request;
    request.pool_handle = vnic.pool_handle;
    request.rx_ring_handle = vnic.rx_ring_handle;
    request.tx_ring_handle = vnic.tx_ring_handle;
    request.mtu = 1500;
    for (unsigned int index = 0; index < 6; index++)
        request.mac[index] = capsule->mac[index];
    request.reserved0[0] = 0;
    request.reserved0[1] = 0;
    for (unsigned int index = 0; index < NET_INTERFACE_ABI_NAME_MAX; index++)
        request.name[index] = 0;
    request.name[0] = 'e';
    request.name[1] = 't';
    request.name[2] = 'h';
    request.name[3] = '0';
    request.interface_handle = 0;
    request.interface_id = 0;
    request.generation = 0;
    request.reserved1 = 0;
    if (mich_net_interface_create(&request) || !request.interface_handle ||
        mich_net_interface_set_link(request.interface_handle,
                                    (capsule->link_status & 1) != 0))
        return -1;
    capsule->interface_handle = request.interface_handle;
    return 0;
}

static int post_rx_buffer_preallocated(struct virtio_net_capsule *capsule,
                                       unsigned int slot,
                                       unsigned long long buffer_id) {
    struct mich_virtqueue_chain_request chain;
    chain.queue_handle = capsule->rx_handle;
    chain.descriptor_count = 1;
    chain.reserved = 0;
    chain.token = 0;
    if (mich_virtqueue_chain_allocate(&chain)) {
        mich_net_interface_driver_release_rx(capsule->interface_handle,
                                             buffer_id);
        return -1;
    }
    struct mich_virtqueue_packet_request packet;
    packet.queue_handle = capsule->rx_handle;
    packet.interface_handle = capsule->interface_handle;
    packet.token = chain.token;
    packet.buffer_id = buffer_id;
    packet.offset = NET_PACKET_HEADROOM - VIRTIO_NET_HEADER_SIZE;
    packet.length = NET_PACKET_DATA_MAX - packet.offset;
    packet.ordinal = 0;
    packet.writable = 1;
    if (mich_virtqueue_set_packet(&packet) ||
        mich_virtqueue_publish(&chain)) {
        mich_virtqueue_chain_release(&chain);
        mich_net_interface_driver_release_rx(capsule->interface_handle,
                                             buffer_id);
        return -1;
    }
    capsule->rx_tokens[slot] = chain.token;
    capsule->rx_buffers[slot] = buffer_id;
    return 0;
}

static int post_rx_buffer(struct virtio_net_capsule *capsule,
                          unsigned int slot) {
    unsigned long long buffer_id =
        mich_net_interface_driver_acquire_rx(capsule->interface_handle);
    if (!buffer_id) return -1;
    return post_rx_buffer_preallocated(capsule, slot, buffer_id);
}

static int post_rx(struct virtio_net_capsule *capsule) {
    for (unsigned int slot = 0; slot < VIRTIO_NET_RX_POSTED; slot++)
        if (post_rx_buffer(capsule, slot)) return -1;
    return 0;
}

static unsigned short ipv4_checksum(const volatile unsigned char *data,
                                    unsigned int length) {
    unsigned int sum = net_checksum_sum(
        0, (const unsigned char *)data, length);
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (unsigned short)~sum;
}

static void write_be32(volatile unsigned char *data, unsigned int value);

static void write_be32(volatile unsigned char *data, unsigned int value);

static int submit_dhcp_frame(struct virtio_net_capsule *capsule,
                             unsigned int message_type,
                             unsigned int requested_address,
                             unsigned int server_address,
                             unsigned long long *token_out,
                             unsigned long long *buffer_out) {
    unsigned long long buffer_id = mich_vnic_acquire_tx(capsule->vnic_handle);
    if (!buffer_id) return -1;
    unsigned int pool_slot = (unsigned int)buffer_id - 1;
    volatile unsigned char *data =
        (volatile unsigned char *)VIRTIO_NET_POOL_ADDRESS +
        (unsigned long long)pool_slot * NET_PACKET_DATA_MAX;
    unsigned int header = NET_PACKET_HEADROOM - VIRTIO_NET_HEADER_SIZE;
    for (unsigned int index = 0; index < VIRTIO_NET_HEADER_SIZE; index++)
        data[header + index] = 0;
    for (unsigned int index = 0; index < 6; index++) {
        data[NET_PACKET_HEADROOM + index] = 0xFF;
        data[NET_PACKET_HEADROOM + 6 + index] = capsule->mac[index];
    }
    for (unsigned int index = 14; index < 342; index++)
        data[NET_PACKET_HEADROOM + index] = 0;
    data[NET_PACKET_HEADROOM + 12] = 0x08;
    data[NET_PACKET_HEADROOM + 13] = 0x00;
    data[NET_PACKET_HEADROOM + 14] = 0x45;
    data[NET_PACKET_HEADROOM + 16] = 0x01;
    data[NET_PACKET_HEADROOM + 17] = 0x48;
    data[NET_PACKET_HEADROOM + 22] = 64;
    data[NET_PACKET_HEADROOM + 23] = 17;
    for (unsigned int index = 30; index < 34; index++)
        data[NET_PACKET_HEADROOM + index] = 0xFF;
    unsigned short checksum = ipv4_checksum(
        data + NET_PACKET_HEADROOM + 14, 20);
    data[NET_PACKET_HEADROOM + 24] = (unsigned char)(checksum >> 8);
    data[NET_PACKET_HEADROOM + 25] = (unsigned char)checksum;
    data[NET_PACKET_HEADROOM + 34] = 0x00;
    data[NET_PACKET_HEADROOM + 35] = 68;
    data[NET_PACKET_HEADROOM + 36] = 0x00;
    data[NET_PACKET_HEADROOM + 37] = 67;
    data[NET_PACKET_HEADROOM + 38] = 0x01;
    data[NET_PACKET_HEADROOM + 39] = 0x34;
    data[NET_PACKET_HEADROOM + 42] = 1;
    data[NET_PACKET_HEADROOM + 43] = 1;
    data[NET_PACKET_HEADROOM + 44] = 6;
    write_be32(data + NET_PACKET_HEADROOM + 46, capsule->dhcp_xid);
    data[NET_PACKET_HEADROOM + 52] = 0x80;
    for (unsigned int index = 0; index < 6; index++)
        data[NET_PACKET_HEADROOM + 70 + index] = capsule->mac[index];
    data[NET_PACKET_HEADROOM + 278] = 99;
    data[NET_PACKET_HEADROOM + 279] = 130;
    data[NET_PACKET_HEADROOM + 280] = 83;
    data[NET_PACKET_HEADROOM + 281] = 99;
    data[NET_PACKET_HEADROOM + 282] = 53;
    data[NET_PACKET_HEADROOM + 283] = 1;
    data[NET_PACKET_HEADROOM + 284] = (unsigned char)message_type;
    unsigned int option = 285;
    if (message_type == 3 && requested_address && server_address) {
        data[NET_PACKET_HEADROOM + option++] = 50;
        data[NET_PACKET_HEADROOM + option++] = 4;
        write_be32(data + NET_PACKET_HEADROOM + option, requested_address);
        option += 4;
        data[NET_PACKET_HEADROOM + option++] = 54;
        data[NET_PACKET_HEADROOM + option++] = 4;
        write_be32(data + NET_PACKET_HEADROOM + option, server_address);
        option += 4;
    }
    data[NET_PACKET_HEADROOM + option++] = 55;
    data[NET_PACKET_HEADROOM + option++] = 3;
    data[NET_PACKET_HEADROOM + option++] = 1;
    data[NET_PACKET_HEADROOM + option++] = 3;
    data[NET_PACKET_HEADROOM + option++] = 6;
    data[NET_PACKET_HEADROOM + option] = 255;
    struct mich_vnic_frame_request frame;
    frame.buffer_id = buffer_id;
    frame.user_address_low = 0;
    frame.user_address_high = 0;
    frame.offset = NET_PACKET_HEADROOM;
    frame.length = 342;
    frame.flags = 0;
    if (mich_vnic_submit_tx(capsule->vnic_handle, &frame)) return -1;
    struct mich_net_packet_descriptor descriptor;
    if (mich_net_interface_driver_dequeue_tx(capsule->interface_handle,
                                             &descriptor) ||
        descriptor.buffer_id != buffer_id)
        return -1;
    struct mich_virtqueue_chain_request chain;
    chain.queue_handle = capsule->tx_handle;
    chain.descriptor_count = 1;
    chain.reserved = 0;
    chain.token = 0;
    if (mich_virtqueue_chain_allocate(&chain)) {
        mich_net_interface_driver_complete_tx(capsule->interface_handle,
                                              buffer_id);
        return -1;
    }
    struct mich_virtqueue_packet_request packet;
    packet.queue_handle = capsule->tx_handle;
    packet.interface_handle = capsule->interface_handle;
    packet.token = chain.token;
    packet.buffer_id = buffer_id;
    packet.offset = descriptor.offset - VIRTIO_NET_HEADER_SIZE;
    packet.length = descriptor.length + VIRTIO_NET_HEADER_SIZE;
    packet.ordinal = 0;
    packet.writable = 0;
    if (mich_virtqueue_set_packet(&packet) ||
        mich_virtqueue_publish(&chain)) {
        mich_virtqueue_chain_release(&chain);
        mich_net_interface_driver_complete_tx(capsule->interface_handle,
                                              buffer_id);
        return -1;
    }
    if (mich_virtqueue_notify(capsule->tx_handle)) return -1;
    *token_out = chain.token;
    *buffer_out = buffer_id;
    return 0;
}

static int complete_tracked_tx(struct virtio_net_capsule *capsule,
                               unsigned long long token) {
    for (unsigned int slot = 0; slot < VIRTIO_NET_TX_OUTSTANDING; slot++) {
        if (capsule->tx_tokens[slot] != token) continue;
        if (mich_net_interface_driver_complete_tx(
                capsule->interface_handle, capsule->tx_buffers[slot]))
            return -1;
        capsule->tx_tokens[slot] = 0;
        capsule->tx_buffers[slot] = 0;
        if (capsule->tx_outstanding) capsule->tx_outstanding--;
        capsule->tx_packets++;
        return 0;
    }
    return -1;
}

static int wait_test_completion(struct virtio_net_capsule *capsule,
                                unsigned long long token,
                                unsigned long long buffer_id) {
    struct mich_virtqueue_completion_result result;
    result.queue_handle = capsule->tx_handle;
    result.length = 0;
    result.token = 0;
    for (unsigned int attempt = 0; attempt < 10000; attempt++) {
        int collected = mich_virtqueue_collect(&result);
        if (collected < 0) return -1;
        if (collected == 1) {
            if (result.token == token) {
                if (mich_net_interface_driver_complete_tx(
                        capsule->interface_handle, buffer_id))
                    return -1;
                capsule->tx_packets++;
                return 0;
            }
            if (complete_tracked_tx(capsule, result.token)) return -1;
        }
        mich_yield();
    }
    return -1;
}

static int arm_dhcp_seconds(struct virtio_net_capsule *capsule,
                            unsigned int seconds) {
    if (!capsule->timer_handle || !seconds) return -1;
    unsigned int maximum = 0x7FFFFFFFu / DHCP_TICKS_PER_SECOND;
    unsigned int ticks = seconds > maximum ? 0x7FFFFFFFu :
        seconds * DHCP_TICKS_PER_SECOND;
    return mich_timer_arm(capsule->timer_handle, ticks, 0);
}

static int transmit_dhcp(struct virtio_net_capsule *capsule,
                         unsigned int message_type) {
    unsigned long long token;
    unsigned long long buffer;
    unsigned int requested = message_type == 3 ?
        capsule->offered_address : 0;
    unsigned int server = message_type == 3 ? capsule->dhcp_server : 0;
    return submit_dhcp_frame(capsule, message_type, requested, server,
                             &token, &buffer) ||
           wait_test_completion(capsule, token, buffer) ? -1 : 0;
}

static void dhcp_timeout(struct virtio_net_capsule *capsule) {
    if (capsule->dhcp_state == DHCP_STATE_BOUND) {
        capsule->dhcp_state = DHCP_STATE_RENEWING;
        capsule->dhcp_retries = 0;
        if (transmit_dhcp(capsule, 3)) capsule->restart_requested = 1;
        unsigned int delay = capsule->dhcp_t2_seconds >
                             capsule->dhcp_t1_seconds ?
            capsule->dhcp_t2_seconds - capsule->dhcp_t1_seconds : 1;
        if (arm_dhcp_seconds(capsule, delay))
            capsule->restart_requested = 1;
        return;
    }
    if (capsule->dhcp_state == DHCP_STATE_RENEWING) {
        capsule->dhcp_state = DHCP_STATE_REBINDING;
        capsule->dhcp_retries = 0;
        if (transmit_dhcp(capsule, 3)) capsule->restart_requested = 1;
        unsigned int delay = capsule->dhcp_lease_seconds >
                             capsule->dhcp_t2_seconds ?
            capsule->dhcp_lease_seconds - capsule->dhcp_t2_seconds : 1;
        if (arm_dhcp_seconds(capsule, delay))
            capsule->restart_requested = 1;
        return;
    }
    if (capsule->dhcp_state == DHCP_STATE_REBINDING ||
        capsule->dhcp_retries >= DHCP_RETRY_MAX) {
        capsule->restart_requested = 1;
        return;
    }
    capsule->dhcp_retries++;
    unsigned int type = capsule->dhcp_state == DHCP_STATE_REQUESTING ? 3 : 1;
    if (transmit_dhcp(capsule, type)) {
        capsule->restart_requested = 1;
        return;
    }
    unsigned int delay = 1u << capsule->dhcp_retries;
    if (arm_dhcp_seconds(capsule, delay)) capsule->restart_requested = 1;
}

static int queue_one_tx_descriptor(struct virtio_net_capsule *capsule,
                                   const struct mich_net_packet_descriptor
                                       *descriptor) {
    if (capsule->tx_outstanding >= VIRTIO_NET_TX_OUTSTANDING) {
        mich_net_interface_driver_complete_tx(
            capsule->interface_handle, descriptor->buffer_id);
        return 0;
    }
    if (descriptor->offset < VIRTIO_NET_HEADER_SIZE) {
        mich_net_interface_driver_complete_tx(
            capsule->interface_handle, descriptor->buffer_id);
        return -1;
    }
    unsigned int tracking = VIRTIO_NET_TX_OUTSTANDING;
    for (unsigned int slot = 0; slot < VIRTIO_NET_TX_OUTSTANDING; slot++)
        if (!capsule->tx_tokens[slot]) {
            tracking = slot;
            break;
        }
    if (tracking == VIRTIO_NET_TX_OUTSTANDING) {
        mich_net_interface_driver_complete_tx(
            capsule->interface_handle, descriptor->buffer_id);
        return -1;
    }
    unsigned int pool_slot = (unsigned int)descriptor->buffer_id - 1;
    volatile unsigned char *data =
        (volatile unsigned char *)VIRTIO_NET_POOL_ADDRESS +
        (unsigned long long)pool_slot * NET_PACKET_DATA_MAX;
    for (unsigned int index = 0; index < VIRTIO_NET_HEADER_SIZE; index++)
        data[descriptor->offset - VIRTIO_NET_HEADER_SIZE + index] = 0;
    struct mich_virtqueue_chain_request chain;
    chain.queue_handle = capsule->tx_handle;
    chain.descriptor_count = 1;
    chain.reserved = 0;
    chain.token = 0;
    if (mich_virtqueue_chain_allocate(&chain)) {
        mich_net_interface_driver_complete_tx(
            capsule->interface_handle, descriptor->buffer_id);
        return -1;
    }
    struct mich_virtqueue_packet_request packet;
    packet.queue_handle = capsule->tx_handle;
    packet.interface_handle = capsule->interface_handle;
    packet.token = chain.token;
    packet.buffer_id = descriptor->buffer_id;
    packet.offset = descriptor->offset - VIRTIO_NET_HEADER_SIZE;
    packet.length = descriptor->length + VIRTIO_NET_HEADER_SIZE;
    packet.ordinal = 0;
    packet.writable = 0;
    if (mich_virtqueue_set_packet(&packet) ||
        mich_virtqueue_publish(&chain)) {
        mich_virtqueue_chain_release(&chain);
        mich_net_interface_driver_complete_tx(
            capsule->interface_handle, descriptor->buffer_id);
        return -1;
    }
    capsule->tx_tokens[tracking] = chain.token;
    capsule->tx_buffers[tracking] = descriptor->buffer_id;
    capsule->tx_outstanding++;
    return 1;
}

static void process_tx_batch(struct virtio_net_capsule *capsule) {
    unsigned long long start = read_cycles();
    // Never dequeue more than the free tracking slots: a frame must not
    // leave the driver ring unless the capsule can track its completion.
    unsigned int available =
        VIRTIO_NET_TX_OUTSTANDING - capsule->tx_outstanding;
    if (!available) return;
    unsigned int maximum = available < VIRTIO_NET_BATCH_BUDGET ?
        available : VIRTIO_NET_BATCH_BUDGET;
    struct mich_net_packet_descriptor descriptors[VIRTIO_NET_BATCH_BUDGET];
    unsigned int dequeued = mich_net_interface_driver_dequeue_tx_batch(
        capsule->interface_handle, maximum, descriptors);
    unsigned int queued = 0;
    for (unsigned int index = 0; index < dequeued; index++)
        if (queue_one_tx_descriptor(capsule, &descriptors[index]) > 0)
            queued++;
    if (queued) {
        mich_virtqueue_notify(capsule->tx_handle);
        unsigned long long cycles = read_cycles() - start;
        capsule->tx_batch_cycles += cycles;
        if (!capsule->tx_batch_reported) {
            capsule->tx_batch_reported = 1;
            write_cycle_metric(
                "Mich virtio-net: TX cycles/packet=0x", cycles / queued);
        }
    }
}

static unsigned int process_tx_completions(
    struct virtio_net_capsule *capsule) {
    struct mich_virtqueue_completion_batch batch;
    batch.queue_handle = capsule->tx_handle;
    batch.maximum = capsule->itr_budget_tx;
    batch.count = 0;
    batch.reserved = 0;
    unsigned long long start = read_cycles();
    if (mich_virtqueue_collect_batch(&batch) || !batch.count) return 0;
    unsigned long long buffer_ids[VIRTIO_NET_BATCH_MAX];
    unsigned int mapped_slots[VIRTIO_NET_BATCH_MAX];
    unsigned int mapped = 0;
    for (unsigned int index = 0; index < batch.count; index++) {
        for (unsigned int slot = 0; slot < VIRTIO_NET_TX_OUTSTANDING; slot++) {
            if (capsule->tx_tokens[slot] != batch.items[index].token)
                continue;
            buffer_ids[mapped] = capsule->tx_buffers[slot];
            mapped_slots[mapped] = slot;
            mapped++;
            break;
        }
    }
    unsigned int completed = 0;
    while (completed < mapped) {
        unsigned int chunk = mapped - completed;
        if (chunk > VIRTIO_NET_DRIVER_BATCH)
            chunk = VIRTIO_NET_DRIVER_BATCH;
        unsigned int done = mich_net_interface_driver_complete_tx_batch(
            capsule->interface_handle, chunk, buffer_ids + completed);
        if (!done) break;
        completed += done;
    }
    for (unsigned int index = 0; index < mapped; index++) {
        if (index >= completed &&
            mich_net_interface_driver_complete_tx(
                capsule->interface_handle, buffer_ids[index]))
            continue;
        unsigned int slot = mapped_slots[index];
        capsule->tx_tokens[slot] = 0;
        capsule->tx_buffers[slot] = 0;
        if (capsule->tx_outstanding) capsule->tx_outstanding--;
        capsule->tx_packets++;
    }
    capsule->tx_batch_cycles += read_cycles() - start;
    return batch.count;
}

static unsigned int read_be32(volatile unsigned char *data) {
    return ((unsigned int)data[0] << 24) |
           ((unsigned int)data[1] << 16) |
           ((unsigned int)data[2] << 8) | data[3];
}

static void write_be32(volatile unsigned char *data, unsigned int value) {
    data[0] = (unsigned char)(value >> 24);
    data[1] = (unsigned char)(value >> 16);
    data[2] = (unsigned char)(value >> 8);
    data[3] = (unsigned char)value;
}

static int parse_dhcp_reply(struct virtio_net_capsule *capsule,
                            unsigned int slot, unsigned int frame_length) {
    unsigned int pool_slot = (unsigned int)capsule->rx_buffers[slot] - 1;
    volatile unsigned char *frame =
        (volatile unsigned char *)VIRTIO_NET_POOL_ADDRESS +
        (unsigned long long)pool_slot * NET_PACKET_DATA_MAX +
        NET_PACKET_HEADROOM;
    if (frame_length < 14 + 20 + 8 + 240 || frame[12] != 0x08 ||
        frame[13] != 0x00)
        return 0;
    unsigned int ihl = (frame[14] & 15u) * 4u;
    if (ihl < 20 || 14 + ihl + 8 + 240 > frame_length ||
        frame[23] != 17)
        return 0;
    volatile unsigned char *udp = frame + 14 + ihl;
    if (udp[0] != 0 || udp[1] != 67 || udp[2] != 0 || udp[3] != 68)
        return 0;
    unsigned int udp_length = ((unsigned int)udp[4] << 8) | udp[5];
    if (udp_length < 8 + 240 || 14 + ihl + udp_length > frame_length)
        return 0;
    volatile unsigned char *dhcp = udp + 8;
    if (dhcp[0] != 2 || dhcp[1] != 1 || dhcp[2] != 6 ||
        read_be32(dhcp + 4) != capsule->dhcp_xid ||
        read_be32(dhcp + 236) != 0x63825363u)
        return 0;
    for (unsigned int index = 0; index < 6; index++)
        if (dhcp[28 + index] != capsule->mac[index]) return 0;
    unsigned int message_type = 0;
    unsigned int netmask = 0;
    unsigned int gateway = 0;
    unsigned int dns = 0;
    unsigned int server = 0;
    unsigned int lease = 0;
    unsigned int renewal = 0;
    unsigned int rebinding = 0;
    unsigned int offset = 240;
    unsigned int payload_length = udp_length - 8;
    while (offset < payload_length) {
        unsigned int code = dhcp[offset++];
        if (code == 0) continue;
        if (code == 255) break;
        if (offset >= payload_length) return 0;
        unsigned int length = dhcp[offset++];
        if (length > payload_length - offset) return 0;
        if (code == 53 && length == 1) message_type = dhcp[offset];
        if (code == 1 && length == 4) netmask = read_be32(dhcp + offset);
        if (code == 3 && length >= 4) gateway = read_be32(dhcp + offset);
        if (code == 6 && length >= 4) dns = read_be32(dhcp + offset);
        if (code == 51 && length == 4) lease = read_be32(dhcp + offset);
        if (code == 54 && length == 4) server = read_be32(dhcp + offset);
        if (code == 58 && length == 4) renewal = read_be32(dhcp + offset);
        if (code == 59 && length == 4) rebinding = read_be32(dhcp + offset);
        offset += length;
    }
    if (message_type == 6) {
        if (!server ||
            (capsule->dhcp_state != DHCP_STATE_REBINDING &&
             capsule->dhcp_server && server != capsule->dhcp_server))
            return 0;
        capsule->dhcp_server = server;
        return 6;
    }
    if (message_type == 5 &&
        (!server ||
         (capsule->dhcp_state != DHCP_STATE_REBINDING &&
          capsule->dhcp_server && server != capsule->dhcp_server)))
        return 0;
    unsigned int offered = read_be32(dhcp + 16);
    if ((message_type != 2 && message_type != 5) || !offered)
        return 0;
    if (message_type == 2 && (!netmask || !gateway || !server)) return 0;
    if (message_type == 5 && capsule->offered_address &&
        offered != capsule->offered_address)
        return 0;
    capsule->offered_address = offered;
    if (netmask) capsule->offered_netmask = netmask;
    if (gateway) capsule->offered_gateway = gateway;
    if (dns) capsule->offered_dns = dns;
    if (server) capsule->dhcp_server = server;
    if (lease) capsule->dhcp_lease_seconds = lease;
    if (renewal) capsule->dhcp_t1_seconds = renewal;
    if (rebinding) capsule->dhcp_t2_seconds = rebinding;
    return (int)message_type;
}

static int rx_completion_valid(struct virtio_net_capsule *capsule,
                               unsigned int slot, unsigned int length) {
    if (length < VIRTIO_NET_HEADER_SIZE + 14 ||
        length > VIRTIO_NET_HEADER_SIZE + 1514)
        return 0;
    unsigned int pool_slot = (unsigned int)capsule->rx_buffers[slot] - 1;
    volatile unsigned char *header =
        (volatile unsigned char *)VIRTIO_NET_POOL_ADDRESS +
        (unsigned long long)pool_slot * NET_PACKET_DATA_MAX +
        NET_PACKET_HEADROOM - VIRTIO_NET_HEADER_SIZE;
    for (unsigned int index = 0; index < 10; index++)
        if (header[index]) return 0;
    return header[10] <= 1 && header[11] == 0;
}

static int process_rx_head(struct virtio_net_capsule *capsule,
                           unsigned long long token,
                           unsigned int length,
                           unsigned int *found_slot,
                           unsigned int *valid) {
    *found_slot = VIRTIO_NET_RX_POSTED;
    *valid = 0;
    for (unsigned int slot = 0; slot < VIRTIO_NET_RX_POSTED; slot++) {
        if (capsule->rx_tokens[slot] != token) continue;
        *found_slot = slot;
        int dhcp_message = 0;
        if (rx_completion_valid(capsule, slot, length)) {
            *valid = 1;
            if (!capsule->rx_seen) {
                capsule->rx_seen = 1;
                mich_write("Mich virtio-net: real RX completion pass\n");
            }
            unsigned int frame_length =
                length - VIRTIO_NET_HEADER_SIZE;
            dhcp_message = parse_dhcp_reply(capsule, slot, frame_length);
            if (dhcp_message == 2 && !capsule->dhcp_offer_seen) {
                capsule->dhcp_offer_seen = 1;
                mich_write("Mich virtio-net: DHCP Offer receive pass\n");
            }
            if (dhcp_message == 6) {
                capsule->restart_requested = 1;
                mich_write("Mich virtio-net: DHCP NAK restart\n");
            }
            if (dhcp_message == 5 && capsule->dhcp_request_sent) {
                if (!capsule->dhcp_lease_seconds)
                    capsule->dhcp_lease_seconds = 3600;
                if (!capsule->dhcp_t1_seconds ||
                    capsule->dhcp_t1_seconds >= capsule->dhcp_lease_seconds)
                    capsule->dhcp_t1_seconds =
                        capsule->dhcp_lease_seconds / 2;
                if (!capsule->dhcp_t2_seconds ||
                    capsule->dhcp_t2_seconds <= capsule->dhcp_t1_seconds ||
                    capsule->dhcp_t2_seconds >= capsule->dhcp_lease_seconds)
                    capsule->dhcp_t2_seconds =
                        capsule->dhcp_lease_seconds -
                        capsule->dhcp_lease_seconds / 8;
                capsule->dhcp_state = DHCP_STATE_BOUND;
                capsule->dhcp_retries = 0;
                if (arm_dhcp_seconds(capsule,
                                     capsule->dhcp_t1_seconds))
                    capsule->restart_requested = 1;
            }
            if (dhcp_message == 5 && capsule->dhcp_request_sent &&
                !capsule->dhcp_ack_seen) {
                capsule->dhcp_ack_seen = 1;
                mich_write("Mich virtio-net: DHCP ACK receive pass\n");
                mich_write("Mich virtio-net: DHCP lease timers pass\n");
                struct mich_net_interface_ipv4_request ipv4;
                ipv4.address = capsule->offered_address;
                ipv4.netmask = capsule->offered_netmask;
                ipv4.gateway = capsule->offered_gateway;
                ipv4.reserved = 0;
                struct mich_net_interface_info info;
                if (!mich_net_interface_configure_ipv4(
                        capsule->interface_handle, &ipv4) &&
                    !mich_net_interface_get_info(
                        capsule->interface_handle, &info) &&
                    info.ipv4_address == ipv4.address &&
                    info.ipv4_netmask == ipv4.netmask) {
                    capsule->ipv4_configured = 1;
                    mich_write("Mich virtio-net: DHCP IPv4 lease applied\n");
                    probes_on_ipv4_up(capsule);
                }
            }
            capsule->rx_packets++;
            return dhcp_message;
        }
        capsule->rx_drops++;
        return 0;
    }
    return -1;
}

static unsigned int process_rx_batch(struct virtio_net_capsule *capsule) {
    struct mich_virtqueue_completion_batch batch;
    batch.queue_handle = capsule->rx_handle;
    batch.maximum = capsule->itr_budget_rx;
    batch.count = 0;
    batch.reserved = 0;
    unsigned long long start = read_cycles();
    if (mich_virtqueue_collect_batch(&batch) || !batch.count) return 0;
    unsigned int slots[VIRTIO_NET_BATCH_MAX];
    unsigned int valid[VIRTIO_NET_BATCH_MAX];
    int dhcp_messages[VIRTIO_NET_BATCH_MAX];
    struct mich_net_interface_buffer_request requests[
        VIRTIO_NET_BATCH_MAX];
    unsigned int request_index[VIRTIO_NET_BATCH_MAX];
    unsigned int request_count = 0;
    unsigned int found_count = 0;
    for (unsigned int index = 0; index < batch.count; index++) {
        slots[index] = VIRTIO_NET_RX_POSTED;
        valid[index] = 0;
        dhcp_messages[index] = 0;
        request_index[index] = 0;
        dhcp_messages[index] = process_rx_head(
            capsule, batch.items[index].token, batch.items[index].length,
            &slots[index], &valid[index]);
        if (dhcp_messages[index] < 0)
            continue;
        found_count++;
        if (!valid[index])
            continue;
        request_index[index] = request_count;
        requests[request_count].buffer_id = capsule->rx_buffers[slots[index]];
        requests[request_count].offset = NET_PACKET_HEADROOM;
        requests[request_count].length =
            batch.items[index].length - VIRTIO_NET_HEADER_SIZE;
        request_count++;
    }
    unsigned int processed = 0;
    while (processed < request_count) {
        unsigned int chunk = request_count - processed;
        if (chunk > VIRTIO_NET_DRIVER_BATCH)
            chunk = VIRTIO_NET_DRIVER_BATCH;
        unsigned int done = mich_net_interface_driver_receive_batch(
            capsule->interface_handle, chunk, requests + processed);
        if (done > chunk) break;
        processed += done;
        // The core has recorded a rejected request but leaves its buffer
        // driver-owned.  Mark it for the release pass, then preserve the
        // remaining valid frames in this bounded batch.
        if (done != chunk) requests[processed++].length = 0;
    }
    // Acquire after the frame is parsed: replacement buffers may reuse
    // pool slots the receive just released.
    unsigned long long replacements[VIRTIO_NET_BATCH_MAX];
    unsigned int replacement_count = 0;
    while (replacement_count < found_count) {
        unsigned int chunk = found_count - replacement_count;
        if (chunk > VIRTIO_NET_DRIVER_BATCH)
            chunk = VIRTIO_NET_DRIVER_BATCH;
        unsigned int acquired = mich_net_interface_driver_acquire_rx_batch(
            capsule->interface_handle, chunk, replacements + replacement_count);
        if (!acquired) break;
        replacement_count += acquired;
    }
    unsigned int replacement_index = 0;
    for (unsigned int index = 0; index < batch.count; index++) {
        unsigned int slot = slots[index];
        if (slot >= VIRTIO_NET_RX_POSTED) continue;
        if (!valid[index] || request_index[index] >= processed ||
            !requests[request_index[index]].length)
            mich_net_interface_driver_release_rx(
                capsule->interface_handle, capsule->rx_buffers[slot]);
        capsule->rx_tokens[slot] = 0;
        capsule->rx_buffers[slot] = 0;
        if (replacement_index < replacement_count)
            post_rx_buffer_preallocated(
                capsule, slot, replacements[replacement_index]);
        else
            post_rx_buffer(capsule, slot);
        replacement_index++;
        if (dhcp_messages[index] == 2 && !capsule->dhcp_request_sent) {
            capsule->dhcp_request_sent = 1;
            unsigned long long token;
            unsigned long long buffer;
            if (!submit_dhcp_frame(
                    capsule, 3, capsule->offered_address,
                    capsule->dhcp_server, &token, &buffer) &&
                !wait_test_completion(capsule, token, buffer)) {
                capsule->dhcp_state = DHCP_STATE_REQUESTING;
                capsule->dhcp_retries = 0;
                arm_dhcp_seconds(capsule, 2);
                mich_write("Mich virtio-net: DHCP Request transmit pass\n");
            }
        }
    }
    mich_virtqueue_notify(capsule->rx_handle);
    unsigned long long cycles = read_cycles() - start;
    capsule->rx_batch_cycles += cycles;
    if (!capsule->batch_reported) {
        capsule->batch_reported = 1;
        mich_write("Mich virtio-net: batched RX/TX datapath pass\n");
        mich_write("Mich virtio-net: packet cycle counters active\n");
        write_cycle_metric(
            "Mich virtio-net: RX cycles/packet=0x", cycles / batch.count);
    }
    return batch.count;
}

static void append_hex(char *text, unsigned int *length,
                       unsigned long long value) {
    for (int shift = 60; shift >= 0; shift -= 4) {
        if (*length >= 140) break;
        unsigned int digit = (unsigned int)(value >> shift) & 15u;
        text[(*length)++] = digit < 10 ? (char)('0' + digit) :
                                     (char)('A' + digit - 10);
    }
}

static void report_itr(struct virtio_net_capsule *capsule) {
    if (capsule->itr_reported ||
        (!capsule->itr_adapt_up && !capsule->itr_adapt_down))
        return;
    capsule->itr_reported = 1;
    mich_write("Mich virtio-net: adaptive interrupt moderation pass\n");
    char text[160];
    unsigned int length = 0;
    const char prefix[] = "Mich virtio-net: adaptive ITR budget rx=0x";
    for (unsigned int index = 0; prefix[index]; index++)
        text[length++] = prefix[index];
    append_hex(text, &length, capsule->itr_budget_rx);
    const char middle[] = " tx=0x";
    for (unsigned int index = 0; middle[index]; index++)
        text[length++] = middle[index];
    append_hex(text, &length, capsule->itr_budget_tx);
    const char tail[] = " masked=0x";
    for (unsigned int index = 0; tail[index]; index++)
        text[length++] = tail[index];
    append_hex(text, &length, capsule->itr_packets_masked);
    text[length++] = '\n';
    text[length] = 0;
    mich_write(text);
}

//
// Adaptive interrupt moderation: the virtio guest counterpart of
// FreeBSD's ixgbe AIM / iflib. The kernel auto-masks every MSI-X on
// delivery, so the used rings are drained while the IRQs stay masked:
// packets that land during processing cannot raise a spurious bridge
// wakeup. Re-arming is deferred until the rings are empty (or the
// per-wake process cap is reached), and the per-wake collect budget is
// adapted from the load snapshot the way the EITR register is adapted
// on real hardware: a full first collect, i.e. work left behind, grows
// the budget so bursts amortize over fewer wakes; an under-used wake
// shrinks it so light traffic keeps low latency; in between it holds.
//
static void itr_drain(struct virtio_net_capsule *capsule) {
    unsigned int rx_total = 0;
    unsigned int tx_total = 0;
    unsigned int rx_first = 0;
    unsigned int tx_first = 0;
    unsigned int passes = 0;
    for (;;) {
        unsigned int rx = process_rx_batch(capsule);
        unsigned int tx = process_tx_completions(capsule);
        if (!passes) {
            rx_first = rx;
            tx_first = tx;
        }
        process_tx_batch(capsule);
        rx_total += rx;
        tx_total += tx;
        passes++;
        if (!rx && !tx) break;
        if (rx_total + tx_total >= VIRTIO_NET_IWR_WAKE_CAP || passes >= 4)
            break;
    }
    capsule->itr_packets_masked += rx_total + tx_total;
    if (rx_first && rx_first >= capsule->itr_budget_rx) {
        unsigned int grown = capsule->itr_budget_rx * 2;
        if (grown > VIRTIO_NET_BATCH_MAX) grown = VIRTIO_NET_BATCH_MAX;
        if (grown > capsule->itr_budget_rx) {
            capsule->itr_budget_rx = grown;
            capsule->itr_adapt_up++;
        }
    } else if (rx_total && rx_total * 2 < capsule->itr_budget_rx) {
        unsigned int shrunk = capsule->itr_budget_rx / 2;
        if (shrunk < VIRTIO_NET_BATCH_MIN) shrunk = VIRTIO_NET_BATCH_MIN;
        if (shrunk < capsule->itr_budget_rx) {
            capsule->itr_budget_rx = shrunk;
            capsule->itr_adapt_down++;
        }
    }
    if (tx_first && tx_first >= capsule->itr_budget_tx) {
        unsigned int grown = capsule->itr_budget_tx * 2;
        if (grown > VIRTIO_NET_BATCH_MAX) grown = VIRTIO_NET_BATCH_MAX;
        if (grown > capsule->itr_budget_tx) {
            capsule->itr_budget_tx = grown;
            capsule->itr_adapt_up++;
        }
    } else if (tx_total && tx_total * 2 < capsule->itr_budget_tx) {
        unsigned int shrunk = capsule->itr_budget_tx / 2;
        if (shrunk < VIRTIO_NET_BATCH_MIN) shrunk = VIRTIO_NET_BATCH_MIN;
        if (shrunk < capsule->itr_budget_tx) {
            capsule->itr_budget_tx = shrunk;
            capsule->itr_adapt_down++;
        }
    }
    report_itr(capsule);
}

#ifndef VIRTIO_NET_SAFE_ARTIFACT
static void restart_test_poll(struct virtio_net_capsule *capsule) {
    if (!capsule->restart_test_enabled || capsule->restart_test_complete)
        return;
    int circuit_fault = (capsule->circuit_test_enabled ||
                         capsule->recovery_test_enabled) &&
        capsule->restart_count == 1;
    if (!circuit_fault && !probes_external_complete(capsule)) return;
    if (!capsule->restart_count)
        mich_write("Mich virtio-net: pre-restart network baseline pass\n");
    if (!capsule->restart_count || circuit_fault) {
        mich_write("Mich virtio-net: restart fault injected\n");
        __asm__ volatile("ud2" ::: "memory");
    }
    capsule->restart_test_complete = 1;
    mich_write("Mich virtio-net: post-restart network baseline pass\n");
}
#endif

static int firmware_probe(void) {
    struct mich_firmware_open_request request;
    request.file_handle = 0;
    request.size = 0;
    request.reserved0 = 0;
    request.reserved1 = 0;
    for (unsigned int index = 0; index < MICH_FIRMWARE_NAME_MAX; index++)
        request.name[index] = 0;
    const char name[] = "virtio-net";
    for (unsigned int index = 0; name[index]; index++)
        request.name[index] = name[index];
    if (mich_firmware_open(&request) || !request.file_handle ||
        request.size < 4)
        return -1;
    struct mich_vfs_io_request read;
    read.file_handle = request.file_handle;
    read.offset = 0;
    read.length = 4;
    read.transferred = 0;
    for (unsigned int index = 0; index < MICH_VFS_IO_MAX; index++)
        read.data[index] = 0;
    if (mich_vfs_read(&read) || read.transferred != 4 ||
        read.data[0] != 0x7F || read.data[1] != 'E' ||
        read.data[2] != 'L' || read.data[3] != 'F' ||
        mich_handle_close(request.file_handle))
        return -1;
    request.file_handle = 0;
    request.size = 0;
    for (unsigned int index = 0; index < MICH_FIRMWARE_NAME_MAX; index++)
        request.name[index] = 0;
    const char denied[] = "init64";
    for (unsigned int index = 0; denied[index]; index++)
        request.name[index] = denied[index];
    return mich_firmware_open(&request) < 0 ? 0 : -1;
}

int main(unsigned long long argument) {
    struct virtio_net_capsule capsule;
    unsigned char *bytes = (unsigned char *)&capsule;
    for (unsigned int index = 0; index < sizeof(capsule); index++)
        bytes[index] = 0;
    if ((unsigned int)argument != 0x564E4554u || bootstrap(&capsule)) return 1;
    capsule.external_probe_enabled = (unsigned int)(argument >> 32) & 1u;
#ifndef VIRTIO_NET_SAFE_ARTIFACT
    capsule.restart_test_enabled = (unsigned int)(argument >> 33) & 1u;
    capsule.circuit_test_enabled = (unsigned int)(argument >> 34) & 1u;
    capsule.recovery_test_enabled = (unsigned int)(argument >> 35) & 1u;
    if ((capsule.circuit_test_enabled && !capsule.restart_test_enabled) ||
        (capsule.recovery_test_enabled &&
         (!capsule.restart_test_enabled || capsule.circuit_test_enabled)) ||
        (capsule.restart_test_enabled && capsule.restart_count > 1))
        return 1;
#else
    if (argument >> 33) return 1;
#endif
    mich_write("Mich virtio-net: bootstrap pass\n");
#ifndef VIRTIO_NET_SAFE_ARTIFACT
    if (capsule.restart_test_enabled && capsule.restart_count == 1)
        mich_write("Mich virtio-net: supervisor restart pass\n");
#else
    mich_write("Mich virtio-net: recovery artifact selected\n");
#endif
    if (firmware_probe()) return 2;
    mich_write("Mich virtio-net: firmware allowlist pass\n");
    mich_write("Mich virtio-net: bounded firmware read pass\n");
    if (negotiate(&capsule)) return 2;
    mich_write("Mich virtio-net: feature negotiation pass\n");
    if (read_config(&capsule)) return 3;
    capsule.dhcp_xid = dhcp_transaction_id(&capsule);
    mich_write("Mich virtio-net: stable device config pass\n");
    int queue_result = create_queues(&capsule);
    if (queue_result) {
        if (queue_result == -1 || queue_result == -4)
            mich_write("Mich virtio-net: queue create failed\n");
        if (queue_result == -2 || queue_result == -5)
            mich_write("Mich virtio-net: queue layout failed\n");
        if (queue_result == -3 || queue_result == -6)
            mich_write("Mich virtio-net: queue map failed\n");
        return 4;
    }
    mich_write("Mich virtio-net: RX and TX queue setup pass\n");
    if (setup_interrupts(&capsule)) return 5;
    mich_write("Mich virtio-net: MSI-X queue vectors pass\n");
    if (setup_interface(&capsule)) return 6;
    mich_write("Mich virtio-net: network interface registered\n");
#ifndef VIRTIO_NET_SAFE_ARTIFACT
    if (capsule.restart_test_enabled && capsule.restart_count == 1)
        mich_write("Mich virtio-net: fresh eth0 re-registration pass\n");
    if ((capsule.circuit_test_enabled || capsule.recovery_test_enabled) &&
        capsule.restart_count == 1)
        restart_test_poll(&capsule);
#endif
    if (post_rx(&capsule)) return 7;
    mich_write("Mich virtio-net: RX buffers published\n");
    if (transition(&capsule, VIRTIO_NET_STATE_QUEUES,
                   VIRTIO_NET_STATE_READY) ||
        mich_virtio_driver_ok(capsule.device_handle) ||
        mich_virtqueue_notify(capsule.rx_handle))
        return 7;
    mich_write("Mich virtio-net: DRIVER_OK pass\n");
    int timer = mich_timer_create();
    if (timer <= 0) return 8;
    capsule.timer_handle = (unsigned int)timer;
    capsule.dhcp_state = DHCP_STATE_SELECTING;
    capsule.dhcp_retries = 0;
    if (arm_dhcp_seconds(&capsule, 1)) return 8;
    mich_write("Mich virtio-net: DHCP retry timer armed\n");
    unsigned long long tx_token;
    unsigned long long tx_buffer;
    if (submit_dhcp_frame(&capsule, 1, 0, 0, &tx_token, &tx_buffer) ||
        wait_test_completion(&capsule, tx_token, tx_buffer))
        return 8;
    mich_write("Mich virtio-net: real TX completion pass\n");
    int ipv6_timer = mich_timer_create();
    if (ipv6_timer <= 0 ||
        mich_net_interface_ipv6_start(capsule.interface_handle) ||
        mich_timer_arm((unsigned int)ipv6_timer, 100, 0))
        return 8;
    capsule.ipv6_timer_handle = (unsigned int)ipv6_timer;
    capsule.ipv6_started = 1;
    mich_write("Mich virtio-net: IPv6 DAD started\n");
    if (transition(&capsule, VIRTIO_NET_STATE_READY,
                   VIRTIO_NET_STATE_RUNNING))
        return 9;
    mich_write("Mich virtio-net: userspace capsule running\n");
    capsule.itr_budget_rx = VIRTIO_NET_BATCH_BUDGET;
    capsule.itr_budget_tx = VIRTIO_NET_BATCH_BUDGET;
    struct mich_wait_many_request waits;
    waits.count = 3;
    waits.timeout = 0;
    for (unsigned int index = 0; index < MICH_WAIT_MANY_MAX; index++)
        waits.handles[index] = 0;
    waits.handles[0] = capsule.bridge_handle;
    waits.handles[1] = capsule.timer_handle;
    waits.handles[2] = capsule.ipv6_timer_handle;
    for (;;) {
        process_tx_completions(&capsule);
        process_tx_batch(&capsule);
        process_rx_batch(&capsule);
        process_tx_batch(&capsule);
        if (probes_poll(&capsule) < 0) return 13;
#ifndef VIRTIO_NET_SAFE_ARTIFACT
        restart_test_poll(&capsule);
#endif
        process_tx_batch(&capsule);
        if (capsule.restart_requested) return 10;
        int ready = mich_wait_many(&waits);
        if (ready == 0) {
            struct mich_bridge_notification notification;
            while (!mich_bridge_read(capsule.bridge_handle, &notification)) {
                if (!capsule.interrupt_seen) {
                    capsule.interrupt_seen = 1;
                    mich_write("Mich virtio-net: interrupt-driven RX/TX pass\n");
                }
            }
            // IRQs stay masked while the used rings drain; re-arm only
            // once the rings are empty (adaptive ITR, see itr_drain).
            itr_drain(&capsule);
            mich_irq_set_mask(capsule.rx_irq_handle, 0);
            mich_irq_set_mask(capsule.tx_irq_handle, 0);
        } else if (ready == 1) {
            dhcp_timeout(&capsule);
        } else if (ready == 2) {
            if (!capsule.ipv6_dad_complete) {
                if (mich_net_interface_ipv6_complete_dad(
                        capsule.interface_handle) ||
                    mich_timer_arm(capsule.ipv6_timer_handle, 100, 100))
                    return 11;
                capsule.ipv6_dad_complete = 1;
                mich_write("Mich virtio-net: external IPv6 DAD pass\n");
                mich_write("Mich virtio-net: IPv6 lifecycle timer pass\n");
            } else if (mich_net_interface_ipv6_maintenance(
                           capsule.interface_handle)) {
                return 11;
            }
        } else {
            return 12;
        }
    }
}
