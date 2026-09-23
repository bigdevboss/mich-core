#include "virtio_blk.h"
#include "virtio_pci.h"
#include "block.h"
#include "pci64.h"
#include "resource.h"

#define VIRTIO_BLK_SLOT_BYTES 1024
#define VIRTIO_BLK_HEADER_BYTES 16
#define VIRTIO_BLK_STATUS_OFF 528
#define VIRTIO_BLK_QUEUE_SIZE 64
#define VIRTIO_BLK_DMA_PAGES 4
#define VIRTIO_BLK_SECTOR_MAX 4096

static void copy_bytes(u8 *dst, const u8 *src, u32 n) {
    for (u32 i = 0; i < n; i++) dst[i] = src[i];
}

static u8 *slot_ptr(struct kernel_object *dma, u32 slot) {
    const struct dma_resource *memory = dma_resource_get(dma);
    if (!memory || !memory->physical || slot >= BLOCK_REQUEST_MAX)
        return 0;
    u32 off = slot * VIRTIO_BLK_SLOT_BYTES;
    if (off + VIRTIO_BLK_SLOT_BYTES > memory->pages * 4096) return 0;
    return (u8 *)(uptr_t)memory->physical + off;
}

static u64 slot_phys(struct kernel_object *dma, u32 slot, u32 extra) {
    const struct dma_resource *memory = dma_resource_get(dma);
    if (!memory) return 0;
    return (u64)memory->physical + slot * VIRTIO_BLK_SLOT_BYTES + extra;
}

static int virtio_blk_issue(struct kernel_object *queue,
                            struct kernel_object *dma, u32 slot, u32 op,
                            u32 lba, const u8 *data, u64 *token) {
    u8 *slot_data = slot_ptr(dma, slot);
    if (!queue || !dma || !data || !token || !slot_data) return -1;
    u32 type = op == BLOCK_OP_WRITE ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    slot_data[0] = (u8)type;
    slot_data[1] = (u8)(type >> 8);
    slot_data[2] = (u8)(type >> 16);
    slot_data[3] = (u8)(type >> 24);
    for (u32 i = 4; i < 8; i++) slot_data[i] = 0;
    u64 sector = lba;
    for (u32 i = 0; i < 8; i++)
        slot_data[8 + i] = (u8)(sector >> (i * 8));
    if (op == BLOCK_OP_WRITE)
        copy_bytes(slot_data + VIRTIO_BLK_HEADER_BYTES, data, BLOCK_SECTOR_SIZE);
    else
        for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++)
            slot_data[VIRTIO_BLK_HEADER_BYTES + i] = 0;
    slot_data[VIRTIO_BLK_STATUS_OFF] = 0xFF;
    u64 chain = 0;
    if (virtqueue_chain_allocate(queue, 3, &chain)) return -1;
    int writable_data = op != BLOCK_OP_WRITE;
    if (virtqueue_descriptor_set(queue, chain, 0, slot_phys(dma, slot, 0),
                                 VIRTIO_BLK_HEADER_BYTES, 0) ||
        virtqueue_descriptor_set(queue, chain, 1,
                                 slot_phys(dma, slot, VIRTIO_BLK_HEADER_BYTES),
                                 BLOCK_SECTOR_SIZE, writable_data) ||
        virtqueue_descriptor_set(queue, chain, 2,
                                 slot_phys(dma, slot, VIRTIO_BLK_STATUS_OFF),
                                 1, 1)) {
        virtqueue_chain_release(queue, chain);
        return -1;
    }
    if (virtqueue_publish(queue, chain)) {
        virtqueue_chain_release(queue, chain);
        return -1;
    }
    if (virtqueue_notify(queue)) return -1;
    *token = chain;
    return 0;
}

static int virtio_blk_reap(struct kernel_object *queue,
                           struct kernel_object *dma, const u64 *tokens,
                           u32 n, u32 *slot, i32 *status, u8 *data) {
    struct virtqueue_completion done;
    if (!queue || !dma || !tokens || !n || !slot || !status || !data)
        return -1;
    int rc = virtqueue_collect(queue, &done);
    if (rc <= 0) return rc;
    u32 found = n;
    for (u32 i = 0; i < n; i++)
        if (tokens[i] == done.token) {
            found = i;
            break;
        }
    if (found == n) return -1;
    u8 *slot_data = slot_ptr(dma, found);
    if (!slot_data) return -1;
    u8 virtio_status = slot_data[VIRTIO_BLK_STATUS_OFF];
    copy_bytes(data, slot_data + VIRTIO_BLK_HEADER_BYTES, BLOCK_SECTOR_SIZE);
    *slot = found;
    *status = virtio_status == VIRTIO_BLK_S_OK ? 0 : -1;
    return 1;
}

struct kernel_object *virtio_blk_open(struct kernel_object *pci) {
    const struct pci_resource *resource = pci_resource_get(pci);
    if (!resource || resource->vendor_id != VIRTIO_PCI_VENDOR ||
        resource->device_id != VIRTIO_PCI_DEVICE_BLK)
        return 0;
    struct kernel_object *device = virtio_pci_create(pci);
    u64 wanted = VIRTIO_FEATURE_VERSION_1 |
                 (1ULL << VIRTIO_BLK_F_RO) |
                 (1ULL << VIRTIO_BLK_F_BLK_SIZE);
    if (!device ||
        virtio_pci_negotiate(device, wanted, VIRTIO_FEATURE_VERSION_1)) {
        if (device) object_release(device);
        return 0;
    }
    u8 config[24];
    u32 generation = 0;
    for (u32 i = 0; i < sizeof(config); i++) config[i] = 0;
    if (virtio_pci_read_config(device, 0, config, 8, &generation)) {
        object_release(device);
        return 0;
    }
    u64 capacity = 0;
    for (u32 i = 0; i < 8; i++) capacity |= (u64)config[i] << (i * 8);
    if (!capacity) {
        object_release(device);
        return 0;
    }
    struct virtio_device_info *info = virtio_pci_get(device);
    if (info && (info->driver_features & (1ULL << VIRTIO_BLK_F_BLK_SIZE))) {
        if (virtio_pci_read_config(device, 0, config, 24, &generation)) {
            object_release(device);
            return 0;
        }
        u32 blk_size = (u32)config[20] | ((u32)config[21] << 8) |
                       ((u32)config[22] << 16) | ((u32)config[23] << 24);
        if (blk_size != BLOCK_SECTOR_SIZE) {
            object_release(device);
            return 0;
        }
    }
    u32 sectors = capacity > VIRTIO_BLK_SECTOR_MAX ?
        VIRTIO_BLK_SECTOR_MAX : (u32)capacity;
    struct kernel_object *queue = virtqueue_create(device, 0,
                                                   VIRTIO_BLK_QUEUE_SIZE);
    struct kernel_object *dma = queue ?
        dma_resource_allocate(VIRTIO_BLK_DMA_PAGES, 0x3FFFFFFFULL) : 0;
    if (!queue || !dma || virtio_pci_set_driver_ok(device)) {
        if (dma) object_release(dma);
        if (queue) object_release(queue);
        object_release(device);
        return 0;
    }
    u32 flags = BLOCK_FLAG_DEFER;
    if (info && (info->driver_features & (1ULL << VIRTIO_BLK_F_RO)))
        flags |= BLOCK_FLAG_READ_ONLY;
    struct kernel_object *block = block_bind_transport(
        sectors, flags, device, queue, dma, virtio_blk_issue, virtio_blk_reap,
        0);
    object_release(dma);
    object_release(queue);
    object_release(device);
    return block;
}
