#include "nvme.h"
#include "pci64.h"
#include "vm64.h"
#include "msix64.h"
#include "resource.h"
#include "object.h"
#include "block.h"

#define NVME_ADMIN_DEPTH 16
#define NVME_IO_DEPTH 64
#define NVME_DMA_PAGES 10

#define NVME_ADMIN_SQ_OFF 0x0000
#define NVME_ADMIN_CQ_OFF 0x1000
#define NVME_IO_SQ_OFF 0x2000
#define NVME_IO_CQ_OFF 0x3000
#define NVME_IDENTIFY_OFF 0x4000
#define NVME_DATA_OFF 0x5000
#define NVME_SGL_OFF 0x7000
#define NVME_SGL_SLOT_BYTES ((NVME_SGL_ENTRY_MAX + 1) * 16)

#define NVME_REG_CAP 0x00
#define NVME_REG_CC 0x14
#define NVME_REG_CSTS 0x1C
#define NVME_REG_AQA 0x24
#define NVME_REG_ASQ 0x28
#define NVME_REG_ACQ 0x30
#define NVME_REG_DBL 0x1000

#define NVME_CC_ENABLE 1u
/* NVMe 2.0 CC layout (QEMU 10): IOSQES bits 16-19, IOCQES bits 20-23. */
#define NVME_CC_IOSQES (6u << 16)
#define NVME_CC_IOCQES (4u << 20)
#define NVME_CSTS_READY 1u

#define NVME_ADMIN_CREATE_SQ 0x01
#define NVME_ADMIN_CREATE_CQ 0x05
#define NVME_ADMIN_IDENTIFY 0x06
#define NVME_IO_WRITE 0x01
#define NVME_IO_READ 0x02

#define NVME_PSDT_SGL (2u << 6)
#define NVME_SGL_DATA_BLOCK 0x00u
#define NVME_SGL_LAST_SEGMENT 0x30u

#define NVME_CLASS_BASE 0x01
#define NVME_SUBCLASS 0x08
#define NVME_PROG_IF 0x02

#define NVME_IDENT_NSID 1
#define NVME_IO_QUEUE_ID 1
#define NVME_MSIX_ENTRY 1

struct nvme_state {
    struct kernel_object *pci;
    struct kernel_object *dma;
    struct kernel_object *msix;
    struct kernel_object *irq;
    void *regs_mapping;
    usize_t regs_mapping_length;
    volatile u8 *regs;
    const struct dma_resource *memory;
    u32 doorbell_stride;
    u32 admin_sq_tail;
    u32 admin_cq_head;
    u32 admin_phase;
    u32 io_sq_tail;
    u32 io_cq_head;
    u32 io_phase;
    u32 sector_count;
    u32 active;
};

static struct nvme_state states[NVME_DEVICE_MAX];

static void copy_bytes(u8 *dst, const u8 *src, u32 n) {
    for (u32 i = 0; i < n; i++) dst[i] = src[i];
}

static void zero_bytes(u8 *dst, u32 n) {
    for (u32 i = 0; i < n; i++) dst[i] = 0;
}

static void put_u16(u8 *dst, u32 offset, u32 value) {
    dst[offset] = (u8)value;
    dst[offset + 1] = (u8)(value >> 8);
}

static void put_u32(u8 *dst, u32 offset, u32 value) {
    for (u32 i = 0; i < 4; i++) dst[offset + i] = (u8)(value >> (i * 8));
}

static void put_u64(u8 *dst, u32 offset, u64 value) {
    for (u32 i = 0; i < 8; i++) dst[offset + i] = (u8)(value >> (i * 8));
}

static u32 get_u32(const u8 *src, u32 offset) {
    return (u32)src[offset] | ((u32)src[offset + 1] << 8) |
           ((u32)src[offset + 2] << 16) | ((u32)src[offset + 3] << 24);
}

static u32 reg_read32(struct nvme_state *s, u32 offset) {
    volatile u32 *word = (volatile u32 *)(s->regs + offset);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return *word;
}

static void reg_write32(struct nvme_state *s, u32 offset, u32 value) {
    volatile u32 *word = (volatile u32 *)(s->regs + offset);
    *word = value;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static u8 *dma_ptr(struct nvme_state *s, u32 offset) {
    if (!s->memory || !s->memory->physical ||
        offset >= s->memory->pages * 4096u)
        return 0;
    return (u8 *)(uptr_t)s->memory->physical + offset;
}

static u64 dma_phys(struct nvme_state *s, u32 offset) {
    return (u64)s->memory->physical + offset;
}

static struct nvme_state *state_by_dma(struct kernel_object *dma) {
    for (u32 index = 0; index < NVME_DEVICE_MAX; index++)
        if (states[index].active && states[index].dma == dma)
            return &states[index];
    return 0;
}

static int wait_csts(struct nvme_state *s, u32 mask, u32 value) {
    for (u32 spin = 0; spin < 50000000u; spin++) {
        if ((reg_read32(s, NVME_REG_CSTS) & mask) == value) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

static void doorbell_write(struct nvme_state *s, u32 queue, u32 completion,
                           u32 value) {
    u32 index = queue * 2 + (completion ? 1 : 0);
    reg_write32(s, NVME_REG_DBL + index * s->doorbell_stride, value);
}

static int admin_sync(struct nvme_state *s, u8 opcode, u32 nsid, u64 dptr,
                      u32 cdw10, u32 cdw11, u32 cdw12) {
    u8 *sq = dma_ptr(s, NVME_ADMIN_SQ_OFF);
    u8 *cq = dma_ptr(s, NVME_ADMIN_CQ_OFF);
    if (!sq || !cq) return -1;
    u32 slot = s->admin_sq_tail;
    u8 *cmd = sq + slot * 64;
    zero_bytes(cmd, 64);
    cmd[0] = opcode;
    put_u16(cmd, 2, slot);
    put_u32(cmd, 4, nsid);
    put_u64(cmd, 24, dptr);
    put_u32(cmd, 40, cdw10);
    put_u32(cmd, 44, cdw11);
    put_u32(cmd, 48, cdw12);
    s->admin_sq_tail = (slot + 1) % NVME_ADMIN_DEPTH;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    doorbell_write(s, 0, 0, s->admin_sq_tail);
    for (u32 spin = 0; spin < 50000000u; spin++) {
        u8 *cqe = cq + s->admin_cq_head * 16;
        u32 dw3 = get_u32(cqe, 12);
        if (((dw3 >> 16) & 1u) == s->admin_phase) {
            __asm__ volatile("pause");
            continue;
        }
        u32 status = (dw3 >> 17) & 0x7FFFu;
        s->admin_cq_head = (s->admin_cq_head + 1) % NVME_ADMIN_DEPTH;
        if (!s->admin_cq_head) s->admin_phase ^= 1;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        doorbell_write(s, 0, 1, s->admin_cq_head);
        if ((dw3 & 0xFFFFu) != slot) return -1;
        return status ? -1 : 0;
    }
    return -1;
}

static void io_submit(struct nvme_state *s, u32 cid, u32 op, u32 lba,
                      u64 dptr, u32 sectors, u32 sgl, u32 sgl_len) {
    u8 *sq = dma_ptr(s, NVME_IO_SQ_OFF);
    u8 *cmd = sq + s->io_sq_tail * 64;
    zero_bytes(cmd, 64);
    cmd[0] = op == BLOCK_OP_WRITE ? NVME_IO_WRITE : NVME_IO_READ;
    if (sgl) cmd[1] = NVME_PSDT_SGL;
    put_u16(cmd, 2, cid);
    put_u32(cmd, 4, NVME_IDENT_NSID);
    put_u64(cmd, 24, dptr);
    if (sgl) {
        // First SGL descriptor is inline in the command: addr@24, len@32,
        // type@39 (Last Segment in the high nibble).
        put_u32(cmd, 32, sgl_len);
        cmd[39] = NVME_SGL_LAST_SEGMENT;
    }
    put_u64(cmd, 40, lba);
    put_u16(cmd, 48, sectors - 1);
    s->io_sq_tail = (s->io_sq_tail + 1) % NVME_IO_DEPTH;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    doorbell_write(s, NVME_IO_QUEUE_ID, 0, s->io_sq_tail);
}

static int nvme_issue(struct kernel_object *queue, struct kernel_object *dma,
                      u32 slot, u32 op, u32 lba, const u8 *data, u64 *token) {
    struct nvme_state *s = state_by_dma(dma);
    u8 *slot_data = s ? dma_ptr(s, NVME_DATA_OFF + slot * 512) : 0;
    (void)queue;
    if (!s || !s->active || !slot_data || !data || !token ||
        slot >= NVME_DATA_SLOT_MAX)
        return -1;
    if (op == BLOCK_OP_WRITE)
        copy_bytes(slot_data, data, 512);
    else
        zero_bytes(slot_data, 512);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    io_submit(s, slot, op, lba, dma_phys(s, NVME_DATA_OFF + slot * 512),
              1, 0, 0);
    *token = slot + 1;
    return 0;
}

static int nvme_issue_sg(struct kernel_object *queue, struct kernel_object *dma,
                         u32 slot, u32 op, u32 lba, struct kernel_object *sg,
                         u64 *token) {
    struct nvme_state *s = state_by_dma(dma);
    struct sg_resource *list = s ? sg_resource_get(sg) : 0;
    u8 *descriptors = s ?
        dma_ptr(s, NVME_SGL_OFF + slot * NVME_SGL_SLOT_BYTES) : 0;
    (void)queue;
    if (!s || !s->active || !list || !sg_resource_usable(sg) ||
        !descriptors || !token || slot >= NVME_DATA_SLOT_MAX ||
        !list->entry_count || list->entry_count > NVME_SGL_ENTRY_MAX)
        return -1;
    u32 bytes = 0;
    for (u32 index = 0; index < list->entry_count; index++)
        bytes += list->entries[index].length;
    if (!bytes || bytes % 512u) return -1;
    for (u32 index = 0; index < list->entry_count; index++) {
        const struct sg_resource_entry *entry = &list->entries[index];
        u8 *descriptor = descriptors + 16 + index * 16;
        put_u64(descriptor, 0, (u64)entry->physical);
        put_u32(descriptor, 8, entry->length);
        descriptor[15] = NVME_SGL_DATA_BLOCK;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    io_submit(s, slot, op, lba,
              dma_phys(s, NVME_SGL_OFF + slot * NVME_SGL_SLOT_BYTES + 16),
              bytes / 512u, 1, list->entry_count * 16u);
    *token = slot + 1;
    return 0;
}

static int nvme_reap(struct kernel_object *queue, struct kernel_object *dma,
                     const u64 *tokens, u32 n, u32 *slot, i32 *status,
                     u8 *data) {
    struct nvme_state *s = state_by_dma(dma);
    u8 *cq = s ? dma_ptr(s, NVME_IO_CQ_OFF) : 0;
    (void)queue;
    if (!s || !s->active || !cq || !tokens || !n || !slot || !status)
        return -1;
    // The MSI-X entry is masked on delivery; re-arm first so a completion
    // racing with this reap can never strand a sleeping waiter.
    if (s->irq) irq_resource_set_mask(s->irq, 0);
    u8 *cqe = cq + s->io_cq_head * 16;
    u32 dw3 = get_u32(cqe, 12);
    if (((dw3 >> 16) & 1u) == s->io_phase) return 0;
    u32 cid = dw3 & 0xFFFFu;
    u32 completion_status = (dw3 >> 17) & 0x7FFFu;
    s->io_cq_head = (s->io_cq_head + 1) % NVME_IO_DEPTH;
    if (!s->io_cq_head) s->io_phase ^= 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    doorbell_write(s, NVME_IO_QUEUE_ID, 1, s->io_cq_head);
    if (cid >= n) return -1;
    u32 found = n;
    for (u32 index = 0; index < n; index++)
        if (tokens[index] == cid + 1) {
            found = index;
            break;
        }
    if (found == n) return -1;
    u8 *slot_data = dma_ptr(s, NVME_DATA_OFF + found * 512);
    if (!slot_data) return -1;
    if (data) copy_bytes(data, slot_data, 512);
    *slot = found;
    *status = completion_status ? -1 : 0;
    return 1;
}

static void nvme_teardown(struct nvme_state *s) {
    if (s->irq) {
        irq_resource_unbind(s->irq);
        object_release(s->irq);
        s->irq = 0;
    }
    if (s->msix) {
        object_release(s->msix);
        s->msix = 0;
    }
    if (s->regs) {
        reg_write32(s, NVME_REG_CC,
                    reg_read32(s, NVME_REG_CC) & ~NVME_CC_ENABLE);
        wait_csts(s, NVME_CSTS_READY, 0);
    }
    if (s->regs_mapping)
        vm64_iounmap(s->regs_mapping, s->regs_mapping_length);
    s->regs_mapping = 0;
    s->regs_mapping_length = 0;
    s->regs = 0;
    if (s->dma) {
        object_release(s->dma);
        s->dma = 0;
    }
    if (s->pci) {
        object_release(s->pci);
        s->pci = 0;
    }
    s->memory = 0;
    s->active = 0;
}

static void nvme_destroy(struct kernel_object *object) {
    if (!object || !object->value || object->value > NVME_DEVICE_MAX) return;
    struct nvme_state *s = &states[object->value - 1];
    if (s->active) nvme_teardown(s);
}

struct kernel_object *nvme_open(struct kernel_object *pci) {
    const struct pci_resource *resource = pci_resource_get(pci);
    if (!resource || resource->class_code != NVME_CLASS_BASE ||
        resource->subclass != NVME_SUBCLASS ||
        resource->programming_interface != NVME_PROG_IF ||
        !resource->msix_offset || !resource->bars[0].address ||
        resource->bars[0].length < 0x2000 || (resource->bars[0].flags & 1))
        return 0;
    for (u32 index = 0; index < NVME_DEVICE_MAX; index++)
        if (states[index].active && states[index].pci == pci) return 0;
    u32 slot = NVME_DEVICE_MAX;
    for (u32 index = 0; index < NVME_DEVICE_MAX; index++)
        if (!states[index].active) {
            slot = index;
            break;
        }
    if (slot == NVME_DEVICE_MAX) return 0;
    struct nvme_state *s = &states[slot];
    struct kernel_object *dma = dma_resource_allocate(NVME_DMA_PAGES,
                                                      0xFFFFFFFFu);
    if (!dma) return 0;
    s->dma = dma;
    s->memory = dma_resource_get(dma);
    u64 physical = resource->bars[0].address;
    usize_t mapping_length =
        ((physical & 0xFFF) + resource->bars[0].length + 0xFFF) &
        ~(usize_t)0xFFF;
    s->regs_mapping = vm64_ioremap(physical & ~0xFFFULL, mapping_length,
                                   VM64_CACHE_UC);
    s->regs = s->regs_mapping ?
        (volatile u8 *)s->regs_mapping + (physical & 0xFFF) : 0;
    if (!s->regs || pci64_set_command(pci, 6, 0)) {
        nvme_teardown(s);
        return 0;
    }
    u64 cap = (u64)reg_read32(s, NVME_REG_CAP) |
              ((u64)reg_read32(s, NVME_REG_CAP + 4) << 32);
    u32 mqes = (u32)(cap & 0xFFFFu);
    s->doorbell_stride = 4u << (u32)((cap >> 32) & 0xFu);
    if (mqes + 1 < NVME_IO_DEPTH || !s->doorbell_stride) {
        nvme_teardown(s);
        return 0;
    }
    if (reg_read32(s, NVME_REG_CSTS) & NVME_CSTS_READY) {
        reg_write32(s, NVME_REG_CC,
                    reg_read32(s, NVME_REG_CC) & ~NVME_CC_ENABLE);
        if (wait_csts(s, NVME_CSTS_READY, 0)) {
            nvme_teardown(s);
            return 0;
        }
    }
    reg_write32(s, NVME_REG_AQA, (NVME_ADMIN_DEPTH - 1) |
                 ((NVME_ADMIN_DEPTH - 1) << 16));
    reg_write32(s, NVME_REG_ASQ, (u32)dma_phys(s, NVME_ADMIN_SQ_OFF));
    reg_write32(s, NVME_REG_ASQ + 4,
                (u32)(dma_phys(s, NVME_ADMIN_SQ_OFF) >> 32));
    reg_write32(s, NVME_REG_ACQ, (u32)dma_phys(s, NVME_ADMIN_CQ_OFF));
    reg_write32(s, NVME_REG_ACQ + 4,
                (u32)(dma_phys(s, NVME_ADMIN_CQ_OFF) >> 32));
    s->admin_sq_tail = 0;
    s->admin_cq_head = 0;
    s->admin_phase = 0;
    reg_write32(s, NVME_REG_CC, NVME_CC_ENABLE | NVME_CC_IOSQES |
                 NVME_CC_IOCQES);
    if (wait_csts(s, NVME_CSTS_READY, NVME_CSTS_READY)) {
        nvme_teardown(s);
        return 0;
    }
    u8 *identify = dma_ptr(s, NVME_IDENTIFY_OFF);
    if (!identify ||
        admin_sync(s, NVME_ADMIN_IDENTIFY, 0,
                   dma_phys(s, NVME_IDENTIFY_OFF), 1, 0, 0) ||
        admin_sync(s, NVME_ADMIN_IDENTIFY, NVME_IDENT_NSID,
                   dma_phys(s, NVME_IDENTIFY_OFF), 0, 0, 0)) {
        nvme_teardown(s);
        return 0;
    }
    u64 nsze = 0;
    for (u32 i = 0; i < 8; i++) nsze |= (u64)identify[i] << (i * 8);
    u32 lbaf = identify[26] & 0xFu;
    if (!nsze || identify[128 + lbaf * 4 + 2] != 9) {
        nvme_teardown(s);
        return 0;
    }
    struct kernel_object *msix = pci64_msix_table_create(pci);
    struct kernel_object *irq = msix ? msix64_create(msix, NVME_MSIX_ENTRY)
                                     : 0;
    // QEMU rejects a Create I/O Completion Queue with a nonzero interrupt
    // vector unless MSI-X is already enabled on the device.
    if (!msix || !irq || pci64_msix_configure(pci, 1, 0)) {
        if (irq) object_release(irq);
        if (msix) object_release(msix);
        nvme_teardown(s);
        return 0;
    }
    s->msix = msix;
    s->irq = irq;
    if (admin_sync(s, NVME_ADMIN_CREATE_CQ, 0,
                   dma_phys(s, NVME_IO_CQ_OFF),
                   NVME_IO_QUEUE_ID | ((NVME_IO_DEPTH - 1) << 16),
                   3u | (NVME_MSIX_ENTRY << 16), 0) ||
        admin_sync(s, NVME_ADMIN_CREATE_SQ, 0,
                   dma_phys(s, NVME_IO_SQ_OFF),
                   NVME_IO_QUEUE_ID | ((NVME_IO_DEPTH - 1) << 16),
                   1u | (NVME_IO_QUEUE_ID << 16), 0)) {
        nvme_teardown(s);
        return 0;
    }
    s->io_sq_tail = 0;
    s->io_cq_head = 0;
    s->io_phase = 0;
    s->sector_count = (u32)nsze;
    if (object_retain(pci)) {
        nvme_teardown(s);
        return 0;
    }
    s->pci = pci;
    struct kernel_object *device = object_create(
        KOBJECT_NVME_DEVICE, slot + 1, nvme_destroy);
    if (!device) {
        nvme_teardown(s);
        return 0;
    }
    struct kernel_object *block = block_bind_transport(
        s->sector_count, 0, device, pci, s->dma, nvme_issue, nvme_reap,
        nvme_issue_sg);
    object_release(device);
    if (!block) {
        nvme_teardown(s);
        return 0;
    }
    struct kernel_object *wait_event = block_wait_event(block);
    if (!wait_event || irq_resource_bind(irq, wait_event) ||
        irq_resource_set_mask(irq, 0)) {
        object_release(block);
        nvme_teardown(s);
        return 0;
    }
    s->active = 1;
    return block;
}

u32 nvme_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < NVME_DEVICE_MAX; index++)
        if (states[index].active) count++;
    return count;
}
