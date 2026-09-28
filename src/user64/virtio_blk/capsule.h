#ifndef VIRTIO_BLK_CAPSULE_H
#define VIRTIO_BLK_CAPSULE_H

#include <virtio/virtio.h>

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

// Fixed guest virtual addresses inside the driver mapping window
// [VM64_DRIVER_BASE, VM64_DRIVER_LIMIT). The BAR window gives each of the up-to
// six BARs a slot the transport maps caps into; the ring DMA, block-layer rings
// and scratch sit above it, none overlapping.
#define VIRTIO_BLK_BAR_WINDOW 0x110000000ULL
#define VIRTIO_BLK_VRING_ADDRESS 0x110800000ULL
#define VIRTIO_BLK_REQ_RING_ADDRESS 0x110900000ULL
#define VIRTIO_BLK_CMP_RING_ADDRESS 0x110910000ULL
#define VIRTIO_BLK_SCRATCH_ADDRESS 0x110920000ULL

// Transport sizing. The pool holds one sector per in-kernel slot (mirrors the
// kernel BLOCK_REQUEST_MAX); the scratch carves a header and status byte per
// slot. The vring grant leaves room to grow the queue to 256.
#define VIRTIO_BLK_QUEUE_SIZE 128u
#define VIRTIO_BLK_VRING_PAGES 2u
#define VIRTIO_BLK_SLOT_MAX 16u
#define VIRTIO_BLK_POOL_PAGES 2u
#define VIRTIO_BLK_RING_CAPACITY BLOCK_DRIVER_REQUEST_MAX
#define VIRTIO_BLK_SCRATCH_STRIDE 32u

struct virtio_blk_capsule {
    unsigned int pci_handle;
    unsigned int bridge_handle;
    unsigned int dma_handle;
    unsigned long long dma_physical;
    unsigned int pool_handle;
    unsigned int request_ring_handle;
    unsigned int completion_ring_handle;
    unsigned int scratch_handle;
    unsigned int block_device_handle;
    unsigned int read_only;
    unsigned long long capacity_sectors;
    unsigned long long negotiated_features;
    struct virtio_device device;
    struct virtqueue queue;
};

#endif
