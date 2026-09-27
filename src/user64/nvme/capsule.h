#ifndef NVME_CAPSULE_H
#define NVME_CAPSULE_H

// Manifest argument the kernel hands the capsule; bring-up fails closed unless it
// matches, so a misrouted spawn cannot drive the device.
#define NVME_CAPSULE_MAGIC 0x4E564D45u

// PCI class triplet for a non-volatile memory controller programmed to NVMe.
#define NVME_CLASS_BASE 0x01u
#define NVME_CLASS_SUB 0x08u
#define NVME_CLASS_PROGIF 0x02u

// Controller register offsets (NVMe base spec 3.1).
#define NVME_REG_CAP 0x00u
#define NVME_REG_CC 0x14u
#define NVME_REG_CSTS 0x1Cu
#define NVME_REG_AQA 0x24u
#define NVME_REG_ASQ 0x28u
#define NVME_REG_ACQ 0x30u
#define NVME_REG_DBL 0x1000u

#define NVME_CC_ENABLE 1u
// QEMU 10 rejects the controller unless the queue entry sizes are set: IOSQES is
// bits 16-19 (2^6 = 64-byte SQE), IOCQES bits 20-23 (2^4 = 16-byte CQE).
#define NVME_CC_IOSQES (6u << 16)
#define NVME_CC_IOCQES (4u << 20)
#define NVME_CSTS_READY 1u

#define NVME_ADMIN_CREATE_SQ 0x01u
#define NVME_ADMIN_CREATE_CQ 0x05u
#define NVME_ADMIN_IDENTIFY 0x06u
#define NVME_IO_WRITE 0x01u
#define NVME_IO_READ 0x02u

#define NVME_ADMIN_DEPTH 16u
#define NVME_IO_DEPTH 64u
#define NVME_IDENT_NSID 1u
#define NVME_IO_QUEUE_ID 1u
#define NVME_SQE_BYTES 64u
#define NVME_CQE_BYTES 16u

// Fixed guest virtual addresses for the capsule's mapped objects, all inside the
// driver mapping window [VM64_DRIVER_BASE, VM64_DRIVER_LIMIT) and non-overlapping.
// The data pool is never mapped: the device DMAs it by physical address.
#define NVME_REGS_ADDRESS 0x110000000ULL
#define NVME_QUEUES_ADDRESS 0x110100000ULL
#define NVME_REQ_RING_ADDRESS 0x110200000ULL
#define NVME_CMP_RING_ADDRESS 0x110210000ULL

// Contiguous DMA layout for the admin and I/O queues and the identify buffer.
#define NVME_ADMIN_SQ_OFF 0x0000u
#define NVME_ADMIN_CQ_OFF 0x1000u
#define NVME_IO_SQ_OFF 0x2000u
#define NVME_IO_CQ_OFF 0x3000u
#define NVME_IDENTIFY_OFF 0x4000u
#define NVME_QUEUES_PAGES 5u

// Transport sizing mirrors the virtio-blk capsule and the in-kernel slot bound.
#define NVME_POOL_PAGES 2u
#define NVME_RING_CAPACITY BLOCK_DRIVER_REQUEST_MAX
#define NVME_SLOT_MAX 16u

struct nvme_capsule {
    unsigned int pci_handle;
    unsigned int bridge_handle;
    unsigned int bar_handle;
    unsigned int dma_handle;
    unsigned int pool_handle;
    unsigned int request_ring_handle;
    unsigned int completion_ring_handle;
    unsigned int block_device_handle;
    unsigned int doorbell_stride;
    unsigned int admin_sq_tail;
    unsigned int admin_cq_head;
    unsigned int admin_phase;
    unsigned int io_sq_tail;
    unsigned int io_cq_head;
    unsigned int io_phase;
    unsigned int sector_count;
    unsigned long long dma_physical;
};

#endif
