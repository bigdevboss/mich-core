#include <mich/syscall.h>
#include <mich/driver.h>
#include <mich/virtio.h>
#include <mich/block.h>

#include "capsule.h"

// Userspace virtio-blk capsule: brings the device up through configuration and
// prints a smoke marker. Rings and serving the kernel block layer come later.

static int transition(struct virtio_blk_capsule *capsule,
                      unsigned int expected, unsigned int next) {
    if (!capsule || capsule->state != expected || next != expected + 1)
        return -1;
    capsule->state = next;
    return 0;
}

static int bootstrap(struct virtio_blk_capsule *capsule) {
    struct mich_driver_bootstrap_info info;
    if (mich_driver_bootstrap(&info) ||
        info.abi_version != MICH_DRIVER_ABI_VERSION ||
        info.size != sizeof(info) || info.vendor_id != 0x1AF4 ||
        (info.device_id != VIRTIO_BLK_DEVICE_MODERN &&
         info.device_id != VIRTIO_BLK_DEVICE_TRANSITIONAL))
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
            capsule->queue_irq_handle = resource->handle;
        if (resource->kind == MICH_DRIVER_RESOURCE_BRIDGE)
            capsule->bridge_handle = resource->handle;
    }
    // Require the manifest's grants up front so an incomplete grant fails at
    // bring-up, not at first I/O.
    if (!capsule->pci_handle || !capsule->bridge_handle ||
        !capsule->queue_irq_handle)
        return -1;
    capsule->restart_count = info.restart_count;
    capsule->device_handle = mich_virtio_open(capsule->pci_handle);
    if ((int)capsule->device_handle <= 0) return -1;
    return transition(capsule, VIRTIO_BLK_STATE_CREATED,
                      VIRTIO_BLK_STATE_BOOTSTRAPPED);
}

static int negotiate(struct virtio_blk_capsule *capsule) {
    struct mich_virtio_feature_request request;
    request.wanted = MICH_VIRTIO_FEATURE_VERSION_1 |
                     VIRTIO_BLK_FEATURE_RO |
                     VIRTIO_BLK_FEATURE_BLK_SIZE;
    request.required = MICH_VIRTIO_FEATURE_VERSION_1;
    request.device_features = 0;
    request.driver_features = 0;
    if (mich_virtio_negotiate(capsule->device_handle, &request) ||
        (request.driver_features & request.required) != request.required)
        return -1;
    capsule->negotiated_features = request.driver_features;
    capsule->read_only =
        (request.driver_features & VIRTIO_BLK_FEATURE_RO) ? 1u : 0u;
    return transition(capsule, VIRTIO_BLK_STATE_BOOTSTRAPPED,
                      VIRTIO_BLK_STATE_FEATURES);
}

static int read_config(struct virtio_blk_capsule *capsule) {
    struct mich_virtio_config_request request;
    request.offset = 0;
    // Capacity is the first 8 bytes; the logical block size sits at offset 20
    // and is only present when BLK_SIZE was negotiated (virtio spec 5.2.4).
    request.length =
        (capsule->negotiated_features & VIRTIO_BLK_FEATURE_BLK_SIZE) ? 24 : 8;
    request.generation = 0;
    request.reserved = 0;
    for (unsigned int index = 0; index < MICH_VIRTIO_CONFIG_DATA_MAX; index++)
        request.data[index] = 0;
    if (mich_virtio_read_config(capsule->device_handle, &request)) return -1;
    unsigned long long capacity = 0;
    for (unsigned int index = 0; index < 8; index++)
        capacity |= (unsigned long long)request.data[index] << (index * 8);
    if (!capacity) return -1;
    capsule->capacity_sectors = capacity;
    if (capsule->negotiated_features & VIRTIO_BLK_FEATURE_BLK_SIZE) {
        unsigned int block_size = (unsigned int)request.data[20] |
                                  ((unsigned int)request.data[21] << 8) |
                                  ((unsigned int)request.data[22] << 16) |
                                  ((unsigned int)request.data[23] << 24);
        // The block layer and ABI are fixed at 512-byte sectors.
        if (block_size != MICH_BLOCK_SECTOR_SIZE) return -1;
        capsule->block_size = block_size;
    } else {
        capsule->block_size = MICH_BLOCK_SECTOR_SIZE;
    }
    return transition(capsule, VIRTIO_BLK_STATE_FEATURES,
                      VIRTIO_BLK_STATE_CONFIG);
}

int main(unsigned long long argument) {
    struct virtio_blk_capsule capsule;
    unsigned char *bytes = (unsigned char *)&capsule;
    for (unsigned int index = 0; index < sizeof(capsule); index++)
        bytes[index] = 0;
    if ((unsigned int)argument != VIRTIO_BLK_MAGIC || bootstrap(&capsule))
        return 1;
    if (negotiate(&capsule)) return 2;
    if (read_config(&capsule)) return 3;
    mich_write("Mich virtio-blk: bootstrap pass\n");
    return 0;
}
