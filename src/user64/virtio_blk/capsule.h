#ifndef VIRTIO_BLK_CAPSULE_H
#define VIRTIO_BLK_CAPSULE_H

#define VIRTIO_BLK_STATE_CREATED 0
#define VIRTIO_BLK_STATE_BOOTSTRAPPED 1
#define VIRTIO_BLK_STATE_FEATURES 2
#define VIRTIO_BLK_STATE_CONFIG 3

// Manifest argument the kernel hands the capsule; bring-up fails closed unless it
// matches, so a misrouted spawn cannot drive the device.
#define VIRTIO_BLK_MAGIC 0x56424C4Bu

// virtio spec 5.2.3 feature bits this capsule acts on.
#define VIRTIO_BLK_FEATURE_RO (1ULL << 5)
#define VIRTIO_BLK_FEATURE_BLK_SIZE (1ULL << 6)

#define VIRTIO_BLK_DEVICE_MODERN 0x1042u
#define VIRTIO_BLK_DEVICE_TRANSITIONAL 0x1001u

// virtio spec 5.2.6 request header: type and 64-bit sector, little-endian on the
// wire regardless of host, plus a trailing status byte the device writes.
#define VIRTIO_BLK_T_IN 0u
#define VIRTIO_BLK_T_OUT 1u
#define VIRTIO_BLK_S_OK 0u
#define VIRTIO_BLK_HEADER_SIZE 16u

// Fixed guest virtual addresses for the capsule's mapped objects. Each capsule
// owns its address space, so any non-overlapping range in the driver mapping
// window [VM64_DRIVER_BASE, VM64_DRIVER_LIMIT) serves.
#define VIRTIO_BLK_QUEUE_ADDRESS 0x110000000ULL
#define VIRTIO_BLK_REQ_RING_ADDRESS 0x110100000ULL
#define VIRTIO_BLK_CMP_RING_ADDRESS 0x110110000ULL
#define VIRTIO_BLK_SCRATCH_ADDRESS 0x110120000ULL

// Transport sizing. The pool holds one sector per in-kernel slot (mirrors the
// kernel BLOCK_REQUEST_MAX); the scratch carves a header and status byte per
// slot. Ring capacity mirrors BLOCK_DRIVER_REQUEST_MAX.
#define VIRTIO_BLK_SLOT_MAX 16u
#define VIRTIO_BLK_POOL_PAGES 2u
#define VIRTIO_BLK_RING_CAPACITY BLOCK_DRIVER_REQUEST_MAX
#define VIRTIO_BLK_SCRATCH_STRIDE 32u

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
    unsigned int pool_handle;
    unsigned int request_ring_handle;
    unsigned int completion_ring_handle;
    unsigned int scratch_handle;
    unsigned int queue_handle;
    unsigned int queue_size;
    unsigned int block_device_handle;
    unsigned long long negotiated_features;
    unsigned long long capacity_sectors;
};

#endif
