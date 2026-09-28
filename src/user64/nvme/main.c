#include <mich/syscall.h>
#include <mich/driver.h>
#include <mich/hardware.h>
#include <mich/ring.h>
#include <mich/memory.h>
#include <mich/event.h>
#include <mich/block.h>

#include "capsule.h"

// Userspace NVMe capsule: brings the controller up over raw hardware handles,
// registers a shared-memory transport with the kernel block layer, and serves
// requests off the request ring by driving NVMe admin/IO queues directly. Data
// moves without a copy: the device DMAs the shared pool in place by physical
// address (PRP1). I/O completions are polled with interrupts disabled (IEN=0),
// so the manifest needs no MSI-X grant. The capsule is the only NVMe driver.

static void write_le16(volatile unsigned char *data, unsigned int value) {
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8);
}

static void write_le32(volatile unsigned char *data, unsigned int value) {
    for (unsigned int index = 0; index < 4; index++)
        data[index] = (unsigned char)(value >> (index * 8));
}

static void write_le64(volatile unsigned char *data, unsigned long long value) {
    write_le32(data, (unsigned int)value);
    write_le32(data + 4, (unsigned int)(value >> 32));
}

static unsigned int read_le32(volatile unsigned char *data) {
    return (unsigned int)data[0] | ((unsigned int)data[1] << 8) |
           ((unsigned int)data[2] << 16) | ((unsigned int)data[3] << 24);
}

// The controller posts a CQE with a DMA write that races this poll, so a
// byte-wise read can straddle it: the phase byte reads new while the command-id
// bytes are still stale, which looks complete but carries the wrong id. Read
// status/phase/id (all in dword 3) until two reads agree, so they come from one
// settled CQE. The in-kernel driver polls at boot without preemption and never
// hits the window; a capsule shares the CPU and does.
static unsigned int cqe_dw3(volatile unsigned char *cqe) {
    for (;;) {
        unsigned int value = read_le32(cqe + 12);
        if (value == read_le32(cqe + 12)) return value;
    }
}

static volatile unsigned char *dma_ptr(unsigned int offset) {
    return (volatile unsigned char *)(NVME_QUEUES_ADDRESS + offset);
}

// The driver-manager grants the DMA region once and reuses it across every
// restart of this capsule without re-zeroing it, so a fresh instance inherits
// the previous one's CQEs. Their phase bits still read 1, which satisfies the
// completion poll before the controller posts anything real, so admin_sync and
// drive_device would accept a stale entry with the wrong command id. Clear the
// queues to restore the phase-0 baseline the NVMe phase protocol assumes.
static void zero_queues(void) {
    volatile unsigned char *base = dma_ptr(0);
    for (unsigned int index = 0; index < NVME_QUEUES_PAGES * 4096u; index++)
        base[index] = 0;
}

static unsigned int reg_read32(unsigned int offset) {
    volatile unsigned int *word =
        (volatile unsigned int *)(NVME_REGS_ADDRESS + offset);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return *word;
}

static void reg_write32(unsigned int offset, unsigned int value) {
    volatile unsigned int *word =
        (volatile unsigned int *)(NVME_REGS_ADDRESS + offset);
    *word = value;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void doorbell_write(struct nvme_capsule *capsule, unsigned int queue,
                           unsigned int completion, unsigned int value) {
    unsigned int index = queue * 2 + (completion ? 1u : 0u);
    reg_write32(NVME_REG_DBL + index * capsule->doorbell_stride, value);
}

static int wait_csts(unsigned int mask, unsigned int value) {
    for (unsigned int spin = 0; spin < 50000000u; spin++)
        if ((reg_read32(NVME_REG_CSTS) & mask) == value) return 0;
    return -1;
}

// Post one admin command and poll the admin CQ for its completion. Serialized:
// the capsule issues admin commands one at a time during bring-up only.
static int admin_sync(struct nvme_capsule *capsule, unsigned int opcode,
                      unsigned int nsid, unsigned long long dptr,
                      unsigned int cdw10, unsigned int cdw11) {
    unsigned int slot = capsule->admin_sq_tail;
    volatile unsigned char *cmd = dma_ptr(NVME_ADMIN_SQ_OFF) + slot * NVME_SQE_BYTES;
    for (unsigned int index = 0; index < NVME_SQE_BYTES; index++) cmd[index] = 0;
    cmd[0] = (unsigned char)opcode;
    write_le16(cmd + 2, slot);
    write_le32(cmd + 4, nsid);
    write_le64(cmd + 24, dptr);
    write_le32(cmd + 40, cdw10);
    write_le32(cmd + 44, cdw11);
    capsule->admin_sq_tail = (slot + 1) % NVME_ADMIN_DEPTH;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    doorbell_write(capsule, 0, 0, capsule->admin_sq_tail);

    volatile unsigned char *cq = dma_ptr(NVME_ADMIN_CQ_OFF);
    for (unsigned int spin = 0; spin < 50000000u; spin++) {
        unsigned int dw3 = cqe_dw3(cq + capsule->admin_cq_head * NVME_CQE_BYTES);
        if (((dw3 >> 16) & 1u) == capsule->admin_phase) continue;
        unsigned int status = (dw3 >> 17) & 0x7FFFu;
        capsule->admin_cq_head = (capsule->admin_cq_head + 1) % NVME_ADMIN_DEPTH;
        if (!capsule->admin_cq_head) capsule->admin_phase ^= 1u;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        doorbell_write(capsule, 0, 1, capsule->admin_cq_head);
        if ((dw3 & 0xFFFFu) != slot) return -1;
        return status ? -1 : 0;
    }
    return -1;
}

static int bootstrap(struct nvme_capsule *capsule) {
    struct mich_driver_bootstrap_info info;
    if (mich_driver_bootstrap(&info) ||
        info.abi_version != MICH_DRIVER_ABI_VERSION ||
        info.size != sizeof(info) ||
        info.class_code != NVME_CLASS_BASE ||
        info.subclass != NVME_CLASS_SUB ||
        info.programming_interface != NVME_CLASS_PROGIF)
        return -1;
    for (unsigned int index = 0; index < info.resource_count; index++) {
        struct mich_driver_resource_info *resource = &info.resources[index];
        if (resource->kind == MICH_DRIVER_RESOURCE_PCI && !resource->index)
            capsule->pci_handle = resource->handle;
        if (resource->kind == MICH_DRIVER_RESOURCE_BAR && !resource->index)
            capsule->bar_handle = resource->handle;
        if (resource->kind == MICH_DRIVER_RESOURCE_DMA) {
            capsule->dma_handle = resource->handle;
            // Bus address the device DMAs the queues through: equals the raw
            // physical without an IOMMU, or the bound IOVA with one.
            capsule->dma_physical = resource->address;
        }
        if (resource->kind == MICH_DRIVER_RESOURCE_BRIDGE)
            capsule->bridge_handle = resource->handle;
    }
    if (!capsule->pci_handle || !capsule->bar_handle ||
        !capsule->dma_handle || !capsule->bridge_handle)
        return -1;
    return 0;
}

// Reset then enable the controller and stand up the admin and I/O queues,
// mirroring the in-kernel nvme.c bring-up order (NVMe base spec 3.5.1).
// Map the manifest-granted BAR and DMA handles and enable the controller. The
// capsule holds no privileged resource capability: the driver manager created
// the BAR mmio object and the contiguous DMA region and handed their handles in
// the bootstrap info, so mapping them needs only the per-handle rights.
static int bring_up(struct nvme_capsule *capsule) {
    if (mich_mmio_map(capsule->bar_handle, NVME_REGS_ADDRESS) ||
        mich_dma_map(capsule->dma_handle, NVME_QUEUES_ADDRESS))
        return -1;

    long command = mich_pci_config_read16(capsule->pci_handle, 4);
    if (command < 0 ||
        mich_pci_set_command(capsule->pci_handle, 4, (unsigned int)command | 6u))
        return -1;

    unsigned int cap_lo = reg_read32(NVME_REG_CAP);
    unsigned int cap_hi = reg_read32(NVME_REG_CAP + 4);
    unsigned int mqes = cap_lo & 0xFFFFu;
    capsule->doorbell_stride = 4u << (cap_hi & 0xFu);
    if (mqes + 1 < NVME_IO_DEPTH || !capsule->doorbell_stride) return -1;

    if (reg_read32(NVME_REG_CSTS) & NVME_CSTS_READY) {
        reg_write32(NVME_REG_CC, reg_read32(NVME_REG_CC) & ~NVME_CC_ENABLE);
        if (wait_csts(NVME_CSTS_READY, 0)) return -1;
    }
    // Only safe once the controller is disabled above: a still-enabled prior
    // instance could DMA a completion into the region mid-clear.
    zero_queues();
    unsigned long long asq = capsule->dma_physical + NVME_ADMIN_SQ_OFF;
    unsigned long long acq = capsule->dma_physical + NVME_ADMIN_CQ_OFF;
    reg_write32(NVME_REG_AQA,
                (NVME_ADMIN_DEPTH - 1) | ((NVME_ADMIN_DEPTH - 1) << 16));
    reg_write32(NVME_REG_ASQ, (unsigned int)asq);
    reg_write32(NVME_REG_ASQ + 4, (unsigned int)(asq >> 32));
    reg_write32(NVME_REG_ACQ, (unsigned int)acq);
    reg_write32(NVME_REG_ACQ + 4, (unsigned int)(acq >> 32));
    reg_write32(NVME_REG_CC, NVME_CC_ENABLE | NVME_CC_IOSQES | NVME_CC_IOCQES);
    if (wait_csts(NVME_CSTS_READY, NVME_CSTS_READY)) return -1;

    unsigned long long identify = capsule->dma_physical + NVME_IDENTIFY_OFF;
    if (admin_sync(capsule, NVME_ADMIN_IDENTIFY, 0, identify, 1, 0) ||
        admin_sync(capsule, NVME_ADMIN_IDENTIFY, NVME_IDENT_NSID, identify, 0, 0))
        return -1;
    volatile unsigned char *ident = dma_ptr(NVME_IDENTIFY_OFF);
    unsigned long long nsze = 0;
    for (unsigned int index = 0; index < 8; index++)
        nsze |= (unsigned long long)ident[index] << (index * 8);
    unsigned int lbaf = ident[26] & 0xFu;
    if (!nsze || ident[128 + lbaf * 4 + 2] != 9) return -1;

    // Create the I/O completion queue with IEN=0 (cdw11 bit1 clear) so no MSI-X
    // vector is needed; completions are polled in drive_device.
    if (admin_sync(capsule, NVME_ADMIN_CREATE_CQ, 0,
                   capsule->dma_physical + NVME_IO_CQ_OFF,
                   NVME_IO_QUEUE_ID | ((NVME_IO_DEPTH - 1) << 16), 1u) ||
        admin_sync(capsule, NVME_ADMIN_CREATE_SQ, 0,
                   capsule->dma_physical + NVME_IO_SQ_OFF,
                   NVME_IO_QUEUE_ID | ((NVME_IO_DEPTH - 1) << 16),
                   1u | (NVME_IO_QUEUE_ID << 16)))
        return -1;
    capsule->sector_count = (unsigned int)nsze;
    return 0;
}

static int setup_transport(struct nvme_capsule *capsule) {
    int pool = mich_shared_memory_create(NVME_POOL_PAGES);
    int req_ring = mich_ring_create(NVME_RING_CAPACITY,
                                    sizeof(struct mich_block_driver_request));
    int cmp_ring = mich_ring_create(NVME_RING_CAPACITY,
                                    sizeof(struct mich_block_driver_completion));
    if (pool <= 0 || req_ring <= 0 || cmp_ring <= 0 ||
        mich_ring_map((unsigned int)req_ring, NVME_REQ_RING_ADDRESS) ||
        mich_ring_map((unsigned int)cmp_ring, NVME_CMP_RING_ADDRESS))
        return -1;
    capsule->pool_handle = (unsigned int)pool;
    capsule->request_ring_handle = (unsigned int)req_ring;
    capsule->completion_ring_handle = (unsigned int)cmp_ring;

    struct mich_block_driver_register_request request;
    unsigned char *bytes = (unsigned char *)&request;
    for (unsigned int index = 0; index < sizeof(request); index++)
        bytes[index] = 0;
    request.request_ring_handle = capsule->request_ring_handle;
    request.completion_ring_handle = capsule->completion_ring_handle;
    request.pool_handle = capsule->pool_handle;
    request.sector_size = MICH_BLOCK_SECTOR_SIZE;
    // The block layer caps a device at BLOCK_SECTOR_MAX sectors, well below a real
    // namespace; expose at most that so registration does not reject the device.
    request.sector_count = capsule->sector_count > BLOCK_SECTOR_MAX
                               ? BLOCK_SECTOR_MAX
                               : capsule->sector_count;
    request.name[0] = 'n';
    request.name[1] = 'v';
    request.name[2] = 'm';
    request.name[3] = 'e';
    if (mich_block_interface_create(&request) || !request.device_handle)
        return -1;
    capsule->block_device_handle = request.device_handle;
    return 0;
}

static volatile unsigned char *ring_descriptor(unsigned long long base,
                                               unsigned long long index) {
    volatile struct mich_ring_shared_header *header =
        (volatile struct mich_ring_shared_header *)base;
    return (volatile unsigned char *)base + header->data_offset +
           (index % header->capacity) * header->descriptor_size;
}

// Submit one I/O command and poll the I/O CQ for it. PRP1 points straight at the
// shared pool slot the kernel filled (write) or will drain (read), so the device
// DMAs it in place and nothing is copied on this path. A single 512-byte sector
// never crosses a page, so PRP2 stays zero.
static int drive_device(struct nvme_capsule *capsule, unsigned int op,
                        unsigned int lba, unsigned int sectors,
                        unsigned int pool_offset, unsigned int slot) {
    if (slot >= NVME_SLOT_MAX || sectors != 1 ||
        (op != MICH_BLOCK_OP_READ && op != MICH_BLOCK_OP_WRITE))
        return -1;
    unsigned long long prp1 =
        mich_resource_physical(capsule->pool_handle, pool_offset);
    if (prp1 == (unsigned long long)-1) return -1;

    volatile unsigned char *cmd =
        dma_ptr(NVME_IO_SQ_OFF) + capsule->io_sq_tail * NVME_SQE_BYTES;
    for (unsigned int index = 0; index < NVME_SQE_BYTES; index++) cmd[index] = 0;
    cmd[0] = (op == MICH_BLOCK_OP_WRITE) ? NVME_IO_WRITE : NVME_IO_READ;
    write_le16(cmd + 2, slot);
    write_le32(cmd + 4, NVME_IDENT_NSID);
    write_le64(cmd + 24, prp1);
    write_le64(cmd + 40, lba);
    write_le16(cmd + 48, sectors - 1);
    capsule->io_sq_tail = (capsule->io_sq_tail + 1) % NVME_IO_DEPTH;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    doorbell_write(capsule, NVME_IO_QUEUE_ID, 0, capsule->io_sq_tail);

    // Spin on the completion queue rather than yielding: the admin path proves a
    // tight poll lets QEMU post the CQE promptly, whereas yielding hands the CPU
    // to other tasks and stretches a sub-millisecond I/O across their run.
    volatile unsigned char *cq = dma_ptr(NVME_IO_CQ_OFF);
    for (unsigned int attempt = 0; attempt < 50000000u; attempt++) {
        unsigned int dw3 = cqe_dw3(cq + capsule->io_cq_head * NVME_CQE_BYTES);
        if (((dw3 >> 16) & 1u) == capsule->io_phase) continue;
        unsigned int cid = dw3 & 0xFFFFu;
        unsigned int status = (dw3 >> 17) & 0x7FFFu;
        capsule->io_cq_head = (capsule->io_cq_head + 1) % NVME_IO_DEPTH;
        if (!capsule->io_cq_head) capsule->io_phase ^= 1u;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        doorbell_write(capsule, NVME_IO_QUEUE_ID, 1, capsule->io_cq_head);
        if (cid != slot) return -1;
        return status ? -1 : 0;
    }
    return -1;
}

// Consume one request the kernel produced, drive it, and publish the completion.
// Returns 1 when a request was handled, 0 when the ring is empty, negative on a
// transport fault (the supervisor then restarts the capsule).
static int serve_request(struct nvme_capsule *capsule) {
    volatile struct mich_ring_shared_header *req =
        (volatile struct mich_ring_shared_header *)NVME_REQ_RING_ADDRESS;
    if (req->producer == req->consumer) return 0;
    volatile struct mich_block_driver_request *entry =
        (volatile struct mich_block_driver_request *)ring_descriptor(
            NVME_REQ_RING_ADDRESS, req->consumer);
    unsigned long long request_id = entry->request_id;
    unsigned int op = entry->op;
    unsigned int lba = entry->lba;
    unsigned int sectors = entry->sectors;
    unsigned int pool_offset = entry->pool_offset;
    if (mich_ring_consume(capsule->request_ring_handle, 1)) return -1;

    unsigned int slot = pool_offset / MICH_BLOCK_SECTOR_SIZE;
    int status = drive_device(capsule, op, lba, sectors, pool_offset, slot);

    volatile struct mich_ring_shared_header *cmp =
        (volatile struct mich_ring_shared_header *)NVME_CMP_RING_ADDRESS;
    volatile struct mich_block_driver_completion *done =
        (volatile struct mich_block_driver_completion *)ring_descriptor(
            NVME_CMP_RING_ADDRESS, cmp->producer);
    done->request_id = request_id;
    done->status = status;
    done->transferred = status ? 0u : sectors * MICH_BLOCK_SECTOR_SIZE;
    if (mich_ring_submit(capsule->completion_ring_handle, 1)) return -1;
    return 1;
}

// Exercise the whole path end to end: as its own client the capsule writes a
// sector and reads it back through the kernel block layer, serving each in
// between, and checks the bytes survive the round trip. This proves the ring
// transport and the pool zero-copy against a real device before clients arrive.
static int self_test(struct nvme_capsule *capsule) {
    if (capsule->sector_count <= 5) return 0;
    struct mich_block_io_request io;
    for (unsigned int index = 0; index < MICH_BLOCK_SECTOR_SIZE; index++)
        io.data[index] = (unsigned char)((index * 7u + 19u) & 0xFFu);
    io.device_handle = capsule->block_device_handle;
    io.op = MICH_BLOCK_OP_WRITE;
    io.lba = 5;
    io.sectors = 1;
    io.request_id = 0;
    io.status = 0;
    io.transferred = 0;
    if (mich_block_submit(&io)) return -1;
    unsigned long long write_id = io.request_id;
    if (serve_request(capsule) != 1 ||
        mich_block_service(capsule->block_device_handle)) return -1;
    io.request_id = write_id;
    if (mich_block_collect(&io) || io.status) return -1;

    for (unsigned int index = 0; index < MICH_BLOCK_SECTOR_SIZE; index++)
        io.data[index] = 0;
    io.device_handle = capsule->block_device_handle;
    io.op = MICH_BLOCK_OP_READ;
    io.lba = 5;
    io.sectors = 1;
    io.request_id = 0;
    if (mich_block_submit(&io)) return -1;
    unsigned long long read_id = io.request_id;
    if (serve_request(capsule) != 1 ||
        mich_block_service(capsule->block_device_handle)) return -1;
    io.request_id = read_id;
    if (mich_block_collect(&io) || io.status ||
        io.transferred != MICH_BLOCK_SECTOR_SIZE)
        return -1;
    for (unsigned int index = 0; index < MICH_BLOCK_SECTOR_SIZE; index++)
        if (io.data[index] != (unsigned char)((index * 7u + 19u) & 0xFFu))
            return -1;
    return 0;
}

// Steady state: drain every queued request, then park on an event nothing
// signals and idle at zero CPU while registered as the live NVMe driver. A
// kernel-to-capsule wakeup doorbell is future work (Variant 2).
static void serve_loop(struct nvme_capsule *capsule) {
    int idle = mich_event_create(MICH_EVENT_MANUAL_RESET, 0);
    if (idle <= 0) return;
    for (;;) {
        int served;
        do {
            served = serve_request(capsule);
            if (served < 0) return;
        } while (served);
        if (mich_event_wait((unsigned int)idle)) return;
    }
}

int main(unsigned long long argument) {
    struct nvme_capsule capsule;
    unsigned char *bytes = (unsigned char *)&capsule;
    for (unsigned int index = 0; index < sizeof(capsule); index++)
        bytes[index] = 0;
    if ((unsigned int)argument != NVME_CAPSULE_MAGIC || bootstrap(&capsule))
        return 1;
    if (bring_up(&capsule)) return 2;
    mich_write("Mich nvme: bootstrap pass\n");
    if (setup_transport(&capsule)) return 3;
    if (self_test(&capsule)) return 4;
    mich_write("Mich nvme: serve pass\n");
    serve_loop(&capsule);
    return 5;
}
