#include <mich/syscall.h>
#include <mich/driver.h>
#include <mich/virtio.h>
#include <mich/net.h>
#include <mich/net_interface.h>
#include <mich/bridge.h>
#include <mich/wait.h>

#define SAFE_VIRTIO_HEADER_SIZE 12
#define SAFE_RX_POSTED 16
#define SAFE_TX_OUTSTANDING 16
#define SAFE_QUEUE_SIZE 256
#define SAFE_RX_ADDRESS 0x110000000ULL
#define SAFE_TX_ADDRESS 0x110010000ULL
#define SAFE_POOL_ADDRESS 0x110020000ULL
#define SAFE_QEMU_ADDRESS 0x0A00020Fu
#define SAFE_QEMU_NETMASK 0xFFFFFF00u
#define SAFE_QEMU_GATEWAY 0x0A000202u
#define SAFE_QEMU_ECHO 0x0A000204u

struct virtio_net_safe {
    unsigned int pci_handle;
    unsigned int device_handle;
    unsigned int rx_handle;
    unsigned int tx_handle;
    unsigned int rx_irq_handle;
    unsigned int tx_irq_handle;
    unsigned int bridge_handle;
    unsigned int interface_handle;
    unsigned int rx_size;
    unsigned int tx_size;
    unsigned int tcp_started;
    unsigned long long negotiated_features;
    unsigned short link_status;
    unsigned char mac[6];
    unsigned long long rx_tokens[SAFE_RX_POSTED];
    unsigned long long rx_buffers[SAFE_RX_POSTED];
    unsigned long long tx_tokens[SAFE_TX_OUTSTANDING];
    unsigned long long tx_buffers[SAFE_TX_OUTSTANDING];
};

static int safe_bootstrap(struct virtio_net_safe *safe) {
    struct mich_driver_bootstrap_info info;
    if (mich_driver_bootstrap(&info) ||
        info.abi_version != MICH_DRIVER_ABI_VERSION ||
        info.size != sizeof(info) || info.vendor_id != 0x1AF4 ||
        info.device_id != 0x1000)
        return -1;
    for (unsigned int index = 0; index < info.resource_count; index++) {
        struct mich_driver_resource_info *resource = &info.resources[index];
        if (resource->kind == MICH_DRIVER_RESOURCE_PCI && !resource->index)
            safe->pci_handle = resource->handle;
        if (resource->kind == MICH_DRIVER_RESOURCE_MSIX_IRQ &&
            resource->index == 1)
            safe->rx_irq_handle = resource->handle;
        if (resource->kind == MICH_DRIVER_RESOURCE_MSIX_IRQ &&
            resource->index == 2)
            safe->tx_irq_handle = resource->handle;
        if (resource->kind == MICH_DRIVER_RESOURCE_BRIDGE)
            safe->bridge_handle = resource->handle;
    }
    if (!safe->pci_handle || !safe->bridge_handle || !safe->rx_irq_handle ||
        !safe->tx_irq_handle)
        return -1;
    safe->device_handle = mich_virtio_open(safe->pci_handle);
    return (int)safe->device_handle > 0 ? 0 : -1;
}

static int safe_negotiate(struct virtio_net_safe *safe) {
    struct mich_virtio_feature_request request;
    request.wanted = MICH_VIRTIO_FEATURE_VERSION_1 |
                     MICH_VIRTIO_NET_FEATURE_MAC |
                     MICH_VIRTIO_NET_FEATURE_STATUS;
    request.required = MICH_VIRTIO_FEATURE_VERSION_1 |
                       MICH_VIRTIO_NET_FEATURE_MAC;
    request.device_features = 0;
    request.driver_features = 0;
    if (mich_virtio_negotiate(safe->device_handle, &request) ||
        (request.driver_features & request.required) != request.required)
        return -1;
    safe->negotiated_features = request.driver_features;
    return 0;
}

static int safe_read_config(struct virtio_net_safe *safe) {
    struct mich_virtio_config_request request;
    request.offset = 0;
    request.length = safe->negotiated_features &
                     MICH_VIRTIO_NET_FEATURE_STATUS ? 8 : 6;
    request.generation = 0;
    request.reserved = 0;
    for (unsigned int index = 0; index < MICH_VIRTIO_CONFIG_DATA_MAX; index++)
        request.data[index] = 0;
    if (mich_virtio_read_config(safe->device_handle, &request)) return -1;
    unsigned int nonzero = 0;
    for (unsigned int index = 0; index < 6; index++) {
        safe->mac[index] = request.data[index];
        nonzero |= request.data[index];
    }
    if (!nonzero) return -1;
    safe->link_status = safe->negotiated_features &
        MICH_VIRTIO_NET_FEATURE_STATUS ?
        (unsigned short)request.data[6] |
        ((unsigned short)request.data[7] << 8) : 1;
    return 0;
}

static int safe_create_queue(struct virtio_net_safe *safe,
                             unsigned short index, unsigned long long address,
                             unsigned int *handle, unsigned int *size) {
    struct mich_virtqueue_create_request request;
    request.device_handle = safe->device_handle;
    request.queue_index = index;
    request.queue_size = SAFE_QUEUE_SIZE;
    request.queue_handle = 0;
    request.descriptor_offset = 0;
    request.available_offset = 0;
    request.used_offset = 0;
    request.total_bytes = 0;
    if (mich_virtqueue_create(&request) || !request.queue_handle ||
        !request.queue_size || request.queue_size > SAFE_QUEUE_SIZE ||
        (request.queue_size & (request.queue_size - 1)) ||
        (request.descriptor_offset & 15) || (request.available_offset & 1) ||
        (request.used_offset & 3) ||
        request.descriptor_offset >= request.available_offset ||
        request.available_offset >= request.used_offset ||
        request.total_bytes <= request.used_offset ||
        mich_virtqueue_map(request.queue_handle, address))
        return -1;
    *handle = request.queue_handle;
    *size = request.queue_size;
    return 0;
}

static int safe_setup_queues(struct virtio_net_safe *safe) {
    if (safe_create_queue(safe, 0, SAFE_RX_ADDRESS,
                          &safe->rx_handle, &safe->rx_size) ||
        safe_create_queue(safe, 1, SAFE_TX_ADDRESS,
                          &safe->tx_handle, &safe->tx_size))
        return -1;
    return mich_virtqueue_set_msix(safe->rx_handle, safe->rx_irq_handle) ||
           mich_virtqueue_set_msix(safe->tx_handle, safe->tx_irq_handle) ||
           mich_irq_bind(safe->rx_irq_handle, safe->bridge_handle) ||
           mich_irq_bind(safe->tx_irq_handle, safe->bridge_handle) ||
           mich_irq_set_mask(safe->rx_irq_handle, 0) ||
           mich_irq_set_mask(safe->tx_irq_handle, 0) ? -1 : 0;
}

static int safe_setup_interface(struct virtio_net_safe *safe) {
    struct mich_vnic_create_request vnic;
    vnic.buffer_count = 32;
    vnic.ring_capacity = 32;
    vnic.vnic_handle = 0;
    vnic.pool_handle = 0;
    vnic.rx_ring_handle = 0;
    vnic.tx_ring_handle = 0;
    if (mich_vnic_create(&vnic) ||
        mich_packet_pool_map(vnic.pool_handle, SAFE_POOL_ADDRESS))
        return -1;
    struct mich_net_interface_create_request request;
    request.pool_handle = vnic.pool_handle;
    request.rx_ring_handle = vnic.rx_ring_handle;
    request.tx_ring_handle = vnic.tx_ring_handle;
    request.mtu = 1500;
    for (unsigned int index = 0; index < 6; index++)
        request.mac[index] = safe->mac[index];
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
                                    (safe->link_status & 1) != 0))
        return -1;
    safe->interface_handle = request.interface_handle;
    return 0;
}

static int safe_post_rx_buffer(struct virtio_net_safe *safe,
                               unsigned int slot) {
    unsigned long long buffer_id =
        mich_net_interface_driver_acquire_rx(safe->interface_handle);
    if (!buffer_id) return -1;
    struct mich_virtqueue_chain_request chain;
    chain.queue_handle = safe->rx_handle;
    chain.descriptor_count = 1;
    chain.reserved = 0;
    chain.token = 0;
    if (mich_virtqueue_chain_allocate(&chain)) {
        mich_net_interface_driver_release_rx(safe->interface_handle, buffer_id);
        return -1;
    }
    struct mich_virtqueue_packet_request packet;
    packet.queue_handle = safe->rx_handle;
    packet.interface_handle = safe->interface_handle;
    packet.token = chain.token;
    packet.buffer_id = buffer_id;
    packet.offset = NET_PACKET_HEADROOM - SAFE_VIRTIO_HEADER_SIZE;
    packet.length = NET_PACKET_DATA_MAX - packet.offset;
    packet.ordinal = 0;
    packet.writable = 1;
    if (mich_virtqueue_set_packet(&packet) ||
        mich_virtqueue_publish(&chain)) {
        mich_virtqueue_chain_release(&chain);
        mich_net_interface_driver_release_rx(safe->interface_handle, buffer_id);
        return -1;
    }
    safe->rx_tokens[slot] = chain.token;
    safe->rx_buffers[slot] = buffer_id;
    return 0;
}

static int safe_post_rx(struct virtio_net_safe *safe) {
    for (unsigned int slot = 0; slot < SAFE_RX_POSTED; slot++)
        if (safe_post_rx_buffer(safe, slot)) return -1;
    return 0;
}

static int safe_track_tx(struct virtio_net_safe *safe,
                         const struct mich_net_packet_descriptor *descriptor) {
    unsigned int slot = SAFE_TX_OUTSTANDING;
    for (unsigned int index = 0; index < SAFE_TX_OUTSTANDING; index++)
        if (!safe->tx_tokens[index]) {
            slot = index;
            break;
        }
    if (slot == SAFE_TX_OUTSTANDING ||
        descriptor->offset < SAFE_VIRTIO_HEADER_SIZE) {
        mich_net_interface_driver_complete_tx(safe->interface_handle,
                                              descriptor->buffer_id);
        return -1;
    }
    unsigned int pool_slot = (unsigned int)descriptor->buffer_id - 1;
    volatile unsigned char *data =
        (volatile unsigned char *)SAFE_POOL_ADDRESS +
        (unsigned long long)pool_slot * NET_PACKET_DATA_MAX;
    for (unsigned int index = 0; index < SAFE_VIRTIO_HEADER_SIZE; index++)
        data[descriptor->offset - SAFE_VIRTIO_HEADER_SIZE + index] = 0;
    struct mich_virtqueue_chain_request chain;
    chain.queue_handle = safe->tx_handle;
    chain.descriptor_count = 1;
    chain.reserved = 0;
    chain.token = 0;
    if (mich_virtqueue_chain_allocate(&chain)) {
        mich_net_interface_driver_complete_tx(safe->interface_handle,
                                              descriptor->buffer_id);
        return -1;
    }
    struct mich_virtqueue_packet_request packet;
    packet.queue_handle = safe->tx_handle;
    packet.interface_handle = safe->interface_handle;
    packet.token = chain.token;
    packet.buffer_id = descriptor->buffer_id;
    packet.offset = descriptor->offset - SAFE_VIRTIO_HEADER_SIZE;
    packet.length = descriptor->length + SAFE_VIRTIO_HEADER_SIZE;
    packet.ordinal = 0;
    packet.writable = 0;
    if (mich_virtqueue_set_packet(&packet) ||
        mich_virtqueue_publish(&chain)) {
        mich_virtqueue_chain_release(&chain);
        mich_net_interface_driver_complete_tx(safe->interface_handle,
                                              descriptor->buffer_id);
        return -1;
    }
    safe->tx_tokens[slot] = chain.token;
    safe->tx_buffers[slot] = descriptor->buffer_id;
    return 0;
}

static int safe_reap_tx(struct virtio_net_safe *safe) {
    for (unsigned int count = 0; count < SAFE_TX_OUTSTANDING; count++) {
        struct mich_virtqueue_completion_result result;
        result.queue_handle = safe->tx_handle;
        result.length = 0;
        result.token = 0;
        int collected = mich_virtqueue_collect(&result);
        if (collected < 0) return -1;
        if (!collected) return 0;
        unsigned int slot = SAFE_TX_OUTSTANDING;
        for (unsigned int index = 0; index < SAFE_TX_OUTSTANDING; index++)
            if (safe->tx_tokens[index] == result.token) {
                slot = index;
                break;
            }
        if (slot == SAFE_TX_OUTSTANDING ||
            mich_net_interface_driver_complete_tx(
                safe->interface_handle, safe->tx_buffers[slot]))
            return -1;
        safe->tx_tokens[slot] = 0;
        safe->tx_buffers[slot] = 0;
    }
    return 0;
}

static int safe_drain_tx(struct virtio_net_safe *safe) {
    unsigned int queued = 0;
    for (unsigned int index = 0; index < SAFE_TX_OUTSTANDING; index++) {
        struct mich_net_packet_descriptor descriptor;
        if (mich_net_interface_driver_dequeue_tx(
                safe->interface_handle, &descriptor))
            break;
        if (safe_track_tx(safe, &descriptor)) return -1;
        queued++;
    }
    return !queued || !mich_virtqueue_notify(safe->tx_handle) ? 0 : -1;
}

static int safe_rx_valid(const struct virtio_net_safe *safe,
                         unsigned int slot, unsigned int length) {
    if (length < SAFE_VIRTIO_HEADER_SIZE + 14 ||
        length > SAFE_VIRTIO_HEADER_SIZE + 1514)
        return 0;
    unsigned int pool_slot = (unsigned int)safe->rx_buffers[slot] - 1;
    volatile unsigned char *header =
        (volatile unsigned char *)SAFE_POOL_ADDRESS +
        (unsigned long long)pool_slot * NET_PACKET_DATA_MAX +
        NET_PACKET_HEADROOM - SAFE_VIRTIO_HEADER_SIZE;
    for (unsigned int index = 0; index < 10; index++)
        if (header[index]) return 0;
    return header[10] <= 1 && !header[11];
}

static int safe_receive(struct virtio_net_safe *safe) {
    unsigned int received = 0;
    for (unsigned int count = 0; count < SAFE_RX_POSTED; count++) {
        struct mich_virtqueue_completion_result result;
        result.queue_handle = safe->rx_handle;
        result.length = 0;
        result.token = 0;
        int collected = mich_virtqueue_collect(&result);
        if (collected < 0) return -1;
        if (!collected) break;
        unsigned int slot = SAFE_RX_POSTED;
        for (unsigned int index = 0; index < SAFE_RX_POSTED; index++)
            if (safe->rx_tokens[index] == result.token) {
                slot = index;
                break;
            }
        if (slot == SAFE_RX_POSTED) return -1;
        unsigned long long buffer_id = safe->rx_buffers[slot];
        if (safe_rx_valid(safe, slot, result.length)) {
            struct mich_net_interface_buffer_request request;
            request.buffer_id = buffer_id;
            request.offset = NET_PACKET_HEADROOM;
            request.length = result.length - SAFE_VIRTIO_HEADER_SIZE;
            if (mich_net_interface_driver_receive(
                    safe->interface_handle, &request))
                mich_net_interface_driver_release_rx(safe->interface_handle,
                                                     buffer_id);
        } else {
            mich_net_interface_driver_release_rx(safe->interface_handle,
                                                 buffer_id);
        }
        safe->rx_tokens[slot] = 0;
        safe->rx_buffers[slot] = 0;
        if (safe_post_rx_buffer(safe, slot)) return -1;
        received++;
    }
    return received && mich_virtqueue_notify(safe->rx_handle) ? -1 : 0;
}

static int safe_configure_ipv4(struct virtio_net_safe *safe) {
    struct mich_net_interface_ipv4_request request;
    request.address = SAFE_QEMU_ADDRESS;
    request.netmask = SAFE_QEMU_NETMASK;
    request.gateway = SAFE_QEMU_GATEWAY;
    request.reserved = 0;
    if (mich_net_interface_configure_ipv4(safe->interface_handle, &request))
        return -1;
    struct mich_net_interface_info info;
    if (mich_net_interface_get_info(safe->interface_handle, &info) ||
        info.ipv4_address != request.address ||
        info.ipv4_netmask != request.netmask)
        return -1;
    return 0;
}

static int safe_loop(struct virtio_net_safe *safe) {
    struct mich_wait_many_request waits;
    waits.count = 1;
    waits.timeout = 0;
    for (unsigned int index = 0; index < MICH_WAIT_MANY_MAX; index++)
        waits.handles[index] = 0;
    waits.handles[0] = safe->bridge_handle;
    for (;;) {
        if (safe_reap_tx(safe) || safe_drain_tx(safe) ||
            safe_receive(safe) || safe_drain_tx(safe))
            return -1;
        if (safe->tcp_started) {
            int complete = mich_net_interface_tcp_probe_poll(
                safe->interface_handle);
            if (complete < 0) return -1;
            if (complete == 1) {
                mich_write("Mich virtio-net safe: external TCP echo pass\n");
                safe->tcp_started = 0;
            }
        }
        if (safe_drain_tx(safe)) return -1;
        if (mich_wait_many(&waits)) return -1;
        struct mich_bridge_notification notification;
        while (!mich_bridge_read(safe->bridge_handle, &notification)) {
        }
        if (mich_irq_set_mask(safe->rx_irq_handle, 0) ||
            mich_irq_set_mask(safe->tx_irq_handle, 0))
            return -1;
    }
}

int main(unsigned long long argument) {
    struct virtio_net_safe safe;
    unsigned char *bytes = (unsigned char *)&safe;
    for (unsigned int index = 0; index < sizeof(safe); index++) bytes[index] = 0;
    if ((unsigned int)argument != 0x564E4554u || !(argument & (1ULL << 32)) ||
        argument >> 33)
        return 1;
    mich_write("Mich virtio-net: recovery artifact selected\n");
    if (safe_bootstrap(&safe)) return 2;
    mich_write("Mich virtio-net safe: bootstrap pass\n");
    if (safe_negotiate(&safe) || safe_read_config(&safe)) return 3;
    mich_write("Mich virtio-net safe: device ready pass\n");
    if (safe_setup_queues(&safe) || safe_setup_interface(&safe)) return 4;
    mich_write("Mich virtio-net safe: eth0 registered pass\n");
    if (safe_post_rx(&safe) || mich_virtio_driver_ok(safe.device_handle) ||
        mich_virtqueue_notify(safe.rx_handle))
        return 5;
    mich_write("Mich virtio-net safe: virtio ready pass\n");
    if (safe_configure_ipv4(&safe)) return 6;
    mich_write("Mich virtio-net safe: static IPv4 configured pass\n");
    if (mich_net_interface_tcp_probe_start(
            safe.interface_handle, SAFE_QEMU_ECHO, 8080))
        return 7;
    safe.tcp_started = 1;
    mich_write("Mich virtio-net safe: external TCP queued\n");
    return safe_loop(&safe) ? 8 : 0;
}
