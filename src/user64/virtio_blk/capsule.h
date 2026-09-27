#ifndef VIRTIO_BLK_CAPSULE_H
#define VIRTIO_BLK_CAPSULE_H

#define VIRTIO_BLK_STATE_CREATED 0
#define VIRTIO_BLK_STATE_BOOTSTRAPPED 1
#define VIRTIO_BLK_STATE_FEATURES 2
#define VIRTIO_BLK_STATE_CONFIG 3

// The kernel passes this in the manifest argument so a capsule started against
// the wrong image (a misrouted spawn) fails closed instead of driving a device
// it was not built for. Value spells "VBLK".
#define VIRTIO_BLK_MAGIC 0x56424C4Bu

// virtio-blk feature bits as masks into the 64-bit feature word (virtio spec
// 5.2.3); only the ones this capsule acts on.
#define VIRTIO_BLK_FEATURE_RO (1ULL << 5)
#define VIRTIO_BLK_FEATURE_BLK_SIZE (1ULL << 6)

// Modern and transitional virtio-blk PCI device ids under vendor 0x1AF4. QEMU
// with a modern virtio-blk-pci presents 0x1042; the transitional id is 0x1001.
#define VIRTIO_BLK_DEVICE_MODERN 0x1042u
#define VIRTIO_BLK_DEVICE_TRANSITIONAL 0x1001u

struct virtio_blk_capsule {
    unsigned int state;
    unsigned int restart_count;
    unsigned int pci_handle;
    unsigned int device_handle;
    unsigned int bridge_handle;
    unsigned int config_irq_handle;
    unsigned int queue_irq_handle;
    unsigned int read_only;
    unsigned int block_size;
    unsigned long long negotiated_features;
    unsigned long long capacity_sectors;
};

#endif
