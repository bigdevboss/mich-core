#include <mich/syscall.h>
#include <mich/driver.h>
#include <virtio/virtio.h>
#include <mich/hardware.h>
#include <mich/net.h>
#include <mich/net_interface.h>
#include <mich/bridge.h>
#include <mich/wait.h>

#define SAFE_VIRTIO_HEADER_SIZE 12
#define SAFE_RX_POSTED 16
#define SAFE_TX_OUTSTANDING 16
#define SAFE_QUEUE_SIZE 128u
#define SAFE_VRING_PAGES 2u
#define SAFE_BAR_WINDOW 0x110000000ULL
#define SAFE_VRING_ADDRESS 0x110800000ULL
#define SAFE_POOL_ADDRESS 0x110900000ULL
#define SAFE_RX_MSIX_ENTRY 1u
#define SAFE_TX_MSIX_ENTRY 2u
#define SAFE_FEATURE_MAC (1ULL << 5)
#define SAFE_FEATURE_STATUS (1ULL << 16)
#define SAFE_QEMU_ADDRESS 0x0A00020Fu
#define SAFE_QEMU_NETMASK 0xFFFFFF00u
#define SAFE_QEMU_GATEWAY 0x0A000202u
#define SAFE_QEMU_ECHO 0x0A000204u

struct virtio_net_safe {
    unsigned int pci_handle;
    unsigned int dma_handle;
    unsigned long long dma_physical;
    struct virtio_device device;
    struct virtqueue rx_queue;
    struct virtqueue tx_queue;
    unsigned int rx_irq_handle;
    unsigned int tx_irq_handle;
    unsigned int bridge_handle;
    unsigned int interface_handle;
    unsigned int pool_handle;
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
        if (resource->kind == MICH_DRIVER_RESOURCE_DMA) {
            safe->dma_handle = resource->handle;
            safe->dma_physical = resource->address;
        }
        if (resource->kind == MICH_DRIVER_RESOURCE_BRIDGE)
            safe->bridge_handle = resource->handle;
    }
    return safe->pci_handle && safe->bridge_handle && safe->dma_handle &&
        safe->rx_irq_handle && safe->tx_irq_handle ? 0 : -1;
}

static int safe_negotiate(struct virtio_net_safe *safe) {
    if (virtio_device_setup(&safe->device, safe->pci_handle, SAFE_BAR_WINDOW))
        return -1;
    unsigned long long wanted = VIRTIO_FEATURE_VERSION_1 |
                                SAFE_FEATURE_MAC | SAFE_FEATURE_STATUS;
    unsigned long long required = VIRTIO_FEATURE_VERSION_1 | SAFE_FEATURE_MAC;
    if (virtio_negotiate(&safe->device, wanted, required)) return -1;
    safe->negotiated_features = safe->device.driver_features;
    return 0;
}

static int safe_read_config(struct virtio_net_safe *safe) {
    unsigned int length =
        safe->negotiated_features & SAFE_FEATURE_STATUS ? 8 : 6;
    unsigned char data[8];
    for (unsigned int index = 0; index < sizeof(data); index++) data[index] = 0;
    if (virtio_read_config(&safe->device, 0, data, length)) return -1;
    unsigned int nonzero = 0;
    for (unsigned int index = 0; index < 6; index++) {
        safe->mac[index] = data[index];
        nonzero |= data[index];
    }
    if (!nonzero) return -1;
    safe->link_status = safe->negotiated_features & SAFE_FEATURE_STATUS ?
        (unsigned short)data[6] | ((unsigned short)data[7] << 8) : 1;
    return 0;
}

static int safe_setup_queues(struct virtio_net_safe *safe) {
    unsigned long long stride = (unsigned long long)SAFE_VRING_PAGES * 4096u;
    if (mich_dma_map(safe->dma_handle, SAFE_VRING_ADDRESS) ||
        virtqueue_setup(&safe->device, &safe->rx_queue, 0, SAFE_QUEUE_SIZE,
                        SAFE_VRING_ADDRESS, safe->dma_physical, stride) ||
        virtqueue_setup(&safe->device, &safe->tx_queue, 1, SAFE_QUEUE_SIZE,
                        SAFE_VRING_ADDRESS + stride, safe->dma_physical + stride,
                        stride))
        return -1;
    return virtqueue_set_msix_vector(&safe->device, 0, SAFE_RX_MSIX_ENTRY) ||
           virtqueue_set_msix_vector(&safe->device, 1, SAFE_TX_MSIX_ENTRY) ||
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
    safe->pool_handle = vnic.pool_handle;
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

// Guest-physical of a byte inside a pool buffer: buffer_id low 32 bits hold
// slot+1 (net_buffer.c) and each buffer is one page, so the kernel resolves the
// bus address the device DMAs the frame through. Zero on an out-of-range offset.
static unsigned long long safe_pool_physical(struct virtio_net_safe *safe,
                                             unsigned long long buffer_id,
                                             unsigned int byte_offset) {
    unsigned int pool_slot = (unsigned int)buffer_id - 1;
    unsigned long long physical = mich_resource_physical(
        safe->pool_handle,
        (unsigned long long)pool_slot * NET_PACKET_DATA_MAX + byte_offset);
    return physical == (unsigned long long)-1 ? 0 : physical;
}

static int safe_post_rx_buffer(struct virtio_net_safe *safe,
                               unsigned int slot) {
    unsigned long long buffer_id =
        mich_net_interface_driver_acquire_rx(safe->interface_handle);
    if (!buffer_id) return -1;
    unsigned int offset = NET_PACKET_HEADROOM - SAFE_VIRTIO_HEADER_SIZE;
    unsigned long long physical = safe_pool_physical(safe, buffer_id, offset);
    unsigned long long token;
    if (!physical || virtqueue_chain_alloc(&safe->rx_queue, 1, &token)) {
        mich_net_interface_driver_release_rx(safe->interface_handle, buffer_id);
        return -1;
    }
    if (virtqueue_descriptor_set(&safe->rx_queue, token, 0, physical,
                                 NET_PACKET_DATA_MAX - offset, 1) ||
        virtqueue_chain_publish(&safe->rx_queue, token)) {
        virtqueue_chain_release(&safe->rx_queue, token);
        mich_net_interface_driver_release_rx(safe->interface_handle, buffer_id);
        return -1;
    }
    safe->rx_tokens[slot] = token;
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
    unsigned int offset = descriptor->offset - SAFE_VIRTIO_HEADER_SIZE;
    for (unsigned int index = 0; index < SAFE_VIRTIO_HEADER_SIZE; index++)
        data[offset + index] = 0;
    unsigned long long physical =
        safe_pool_physical(safe, descriptor->buffer_id, offset);
    unsigned long long token;
    if (!physical || virtqueue_chain_alloc(&safe->tx_queue, 1, &token)) {
        mich_net_interface_driver_complete_tx(safe->interface_handle,
                                              descriptor->buffer_id);
        return -1;
    }
    if (virtqueue_descriptor_set(&safe->tx_queue, token, 0, physical,
                                 descriptor->length + SAFE_VIRTIO_HEADER_SIZE,
                                 0) ||
        virtqueue_chain_publish(&safe->tx_queue, token)) {
        virtqueue_chain_release(&safe->tx_queue, token);
        mich_net_interface_driver_complete_tx(safe->interface_handle,
                                              descriptor->buffer_id);
        return -1;
    }
    safe->tx_tokens[slot] = token;
    safe->tx_buffers[slot] = descriptor->buffer_id;
    return 0;
}

static int safe_reap_tx(struct virtio_net_safe *safe) {
    for (unsigned int count = 0; count < SAFE_TX_OUTSTANDING; count++) {
        unsigned long long token;
        unsigned int length;
        int collected = virtqueue_collect(&safe->tx_queue, &token, &length);
        if (collected < 0) return -1;
        if (!collected) return 0;
        unsigned int slot = SAFE_TX_OUTSTANDING;
        for (unsigned int index = 0; index < SAFE_TX_OUTSTANDING; index++)
            if (safe->tx_tokens[index] == token) {
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
    return !queued || !virtqueue_kick(&safe->tx_queue) ? 0 : -1;
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
        unsigned long long token;
        unsigned int length;
        int collected = virtqueue_collect(&safe->rx_queue, &token, &length);
        if (collected < 0) return -1;
        if (!collected) break;
        unsigned int slot = SAFE_RX_POSTED;
        for (unsigned int index = 0; index < SAFE_RX_POSTED; index++)
            if (safe->rx_tokens[index] == token) {
                slot = index;
                break;
            }
        if (slot == SAFE_RX_POSTED) return -1;
        unsigned long long buffer_id = safe->rx_buffers[slot];
        if (safe_rx_valid(safe, slot, length)) {
            struct mich_net_interface_buffer_request request;
            request.buffer_id = buffer_id;
            request.offset = NET_PACKET_HEADROOM;
            request.length = length - SAFE_VIRTIO_HEADER_SIZE;
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
    return received && virtqueue_kick(&safe->rx_queue) ? -1 : 0;
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
    if (safe_post_rx(&safe) || virtio_driver_ok(&safe.device) ||
        virtqueue_kick(&safe.rx_queue))
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
