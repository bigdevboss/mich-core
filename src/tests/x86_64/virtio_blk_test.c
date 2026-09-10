#include "types.h"
#include "object.h"
#include "resource.h"
#include "pci64.h"
#include "virtio_pci.h"
#include "virtio_blk.h"
#include "block.h"

int test_virtio_blk64(void) {
    u32 objects = object_active_count();
    u32 devices = block_active_count();
    u32 virtio = virtio_pci_active_count();
    u32 queues = virtqueue_active_count();
    struct kernel_object *pci = 0;
    struct kernel_object *other = 0;
    for (u32 i = 0; i < pci64_count(); i++) {
        struct kernel_object *cand = pci64_object(i);
        const struct pci_resource *info = pci_resource_get(cand);
        if (!info) continue;
        if (!pci && info->vendor_id == VIRTIO_PCI_VENDOR &&
            info->device_id == VIRTIO_PCI_DEVICE_BLK)
            pci = cand;
        if (!other && (info->vendor_id != VIRTIO_PCI_VENDOR ||
                       info->device_id != VIRTIO_PCI_DEVICE_BLK))
            other = cand;
    }
    if (!pci) return 1;
    u8 pattern[BLOCK_SECTOR_SIZE];
    u8 got[BLOCK_SECTOR_SIZE];
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++) {
        pattern[i] = (u8)(0xA5 ^ i);
        got[i] = 0;
    }
    struct kernel_object *dev = virtio_blk_open(pci);
    struct block_info info;
    int valid = dev && !virtio_blk_open(other) && !virtio_blk_open(pci) &&
        !block_info(dev, &info) && info.sector_size == BLOCK_SECTOR_SIZE &&
        info.sector_count && (info.flags & BLOCK_FLAG_DEFER) &&
        !block_io(dev, BLOCK_OP_WRITE, 5, 1, pattern, BLOCK_SECTOR_SIZE) &&
        !block_io(dev, BLOCK_OP_READ, 5, 1, got, BLOCK_SECTOR_SIZE);
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++)
        if (got[i] != pattern[i]) valid = 0;
    valid = valid && !block_io(dev, BLOCK_OP_READ, 0, 1, got,
                               BLOCK_SECTOR_SIZE);
    valid = valid && !block_revoke(dev) &&
        block_io(dev, BLOCK_OP_READ, 5, 1, got, BLOCK_SECTOR_SIZE) < 0;
    if (dev) object_release(dev);
    valid = valid && object_active_count() == objects &&
        block_active_count() == devices &&
        virtio_pci_active_count() == virtio &&
        virtqueue_active_count() == queues;
    return valid ? 0 : -1;
}
