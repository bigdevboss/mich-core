#include "vtd64.h"
#include "acpi64.h"
#include "vm64.h"
#include "pmm.h"
#include "resource.h"
#include "iommu.h"

#define VTD64_UNIT_MAX ACPI_MAX_DMAR_UNITS
#define VTD64_DOMAIN_MAX 8
#define VTD64_MAPPING_MAX 8
#define VTD64_TABLE_PAGE_MAX 16
#define VTD64_CONTEXT_MAX 16
#define VTD64_REG_VERSION 0x00
#define VTD64_REG_CAP 0x08
#define VTD64_REG_ECAP 0x10
#define VTD64_REG_GCMD 0x18
#define VTD64_REG_GSTS 0x1C
#define VTD64_REG_RTADDR 0x20
#define VTD64_REG_FSTS 0x34
#define VTD64_REG_IQT 0x88
#define VTD64_REG_IQA 0x90
#define VTD64_REG_IRTA 0xB8
#define VTD64_GCMD_TE (1u << 31)
#define VTD64_GCMD_SRTP (1u << 30)
#define VTD64_GCMD_QIE (1u << 26)
#define VTD64_GCMD_IRE (1u << 25)
#define VTD64_GCMD_SIRTP (1u << 24)
#define VTD64_GSTS_TES (1u << 31)
#define VTD64_GSTS_RTPS (1u << 30)
#define VTD64_GSTS_QIES (1u << 26)
#define VTD64_GSTS_IRES (1u << 25)
#define VTD64_GSTS_IRTPS (1u << 24)
#define VTD64_ECAP_IR (1ULL << 3)
#define VTD64_QI_DESCRIPTORS 256
#define VTD64_QI_CC 0x1ULL
#define VTD64_QI_IOTLB 0x2ULL
#define VTD64_QI_IEC 0x4ULL
#define VTD64_QI_WAIT 0x5ULL
#define VTD64_QI_GLOBAL (1ULL << 4)
#define VTD64_QI_IOTLB_DRAIN ((1ULL << 6) | (1ULL << 7))
#define VTD64_QI_WAIT_SW (1ULL << 5)
#define VTD64_QI_WAIT_FN (1ULL << 6)
#define VTD64_QI_STATUS 0x1u
#define VTD64_FAULT_VALID (1ULL << 63)
#define VTD64_FAULT_WRITE (1ULL << 62)
#define VTD64_WAIT_MAX 1000000
#define VTD64_IOVA_BASE 0x00100000ULL
#define VTD64_IOVA_STRIDE 0x01000000ULL
#define VTD64_PAGE_READ 1ULL
#define VTD64_PAGE_WRITE 2ULL
#define VTD64_IR_ENTRIES 256
#define VTD64_IR_SIZE 7
#define VTD64_IRTE_PRESENT (1ULL << 0)
#define VTD64_IRTE_TRIGGER_LEVEL (1ULL << 4)
#define VTD64_IRTE_SVT_SID (1ULL << 18)

struct vtd64_unit {
    volatile u8 *regs;
    paddr_t root;
    paddr_t contexts[VTD64_CONTEXT_MAX];
    u8 context_bus[VTD64_CONTEXT_MAX];
    paddr_t qi_queue;
    u32 qi_tail;
    u64 cap;
    u64 ecap;
    u16 segment;
    u8 flags;
    u32 gcmd;
};

struct vtd64_mapping {
    u64 iova;
    paddr_t physical;
    u32 pages;
    u32 active;
};

struct vtd64_domain {
    u32 owner;
    u16 did;
    u16 segment;
    u8 bus;
    u8 device;
    u8 function;
    u32 unit_mask;
    paddr_t root;
    paddr_t table_pages[VTD64_TABLE_PAGE_MAX];
    u32 table_page_count;
    struct vtd64_mapping mappings[VTD64_MAPPING_MAX];
    u64 next_iova;
    u32 suspended;
    u32 active;
};

static struct vtd64_unit units[VTD64_UNIT_MAX];
static struct vtd64_domain domains[VTD64_DOMAIN_MAX];
static paddr_t qi_status;
static u32 unit_count;
static u32 host_width;
static int detected;
static int translation_enabled;
static u32 fault_count;
static paddr_t ir_table;
static int ir_enabled;
static u8 ir_used[VTD64_IR_ENTRIES];

static u32 reg32(const volatile u8 *base, u32 offset) {
    return *(const volatile u32 *)(base + offset);
}

static u64 reg64(const volatile u8 *base, u32 offset) {
    return *(const volatile u64 *)(base + offset);
}

static void write32(volatile u8 *base, u32 offset, u32 value) {
    *(volatile u32 *)(base + offset) = value;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void write64(volatile u8 *base, u32 offset, u64 value) {
    *(volatile u64 *)(base + offset) = value;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static int wait32(volatile u8 *base, u32 offset, u32 mask, u32 value) {
    for (u32 i = 0; i < VTD64_WAIT_MAX; i++)
        if ((reg32(base, offset) & mask) == value) return 0;
    return -1;
}

static u64 *page(paddr_t physical) {
    return (u64 *)(uptr_t)physical;
}

static void zero_page(paddr_t physical) {
    u64 *p = page(physical);
    for (u32 i = 0; i < 512; i++) p[i] = 0;
}

static void flush_page(paddr_t physical) {
    u8 *p = (u8 *)(uptr_t)physical;
    for (u32 i = 0; i < 4096; i += 64)
        __asm__ volatile("clflush (%0)" : : "r"(p + i) : "memory");
}

static void flush_tables(u32 mask) {
    for (u32 i = 0; i < unit_count; i++) {
        if (!(mask & (1u << i))) continue;
        flush_page(units[i].root);
        for (u32 n = 0; n < VTD64_CONTEXT_MAX; n++)
            if (units[i].contexts[n]) flush_page(units[i].contexts[n]);
    }
    for (u32 i = 0; i < VTD64_DOMAIN_MAX; i++) {
        struct vtd64_domain *d = &domains[i];
        if (!d->active || !(d->unit_mask & mask)) continue;
        for (u32 n = 0; n < d->table_page_count; n++)
            flush_page(d->table_pages[n]);
    }
    __asm__ volatile("mfence" : : : "memory");
}

static void reset_domains(void) {
    for (u32 i = 0; i < VTD64_DOMAIN_MAX; i++) {
        struct vtd64_domain *d = &domains[i];
        if (d->active)
            for (u32 n = 0; n < d->table_page_count; n++)
                pmm_free_page(d->table_pages[n]);
        d->owner = 0;
        d->did = 0;
        d->segment = 0;
        d->bus = 0;
        d->device = 0;
        d->function = 0;
        d->unit_mask = 0;
        d->root = 0;
        d->table_page_count = 0;
        d->next_iova = 0;
        d->suspended = 0;
        d->active = 0;
        for (u32 n = 0; n < VTD64_MAPPING_MAX; n++)
            d->mappings[n].active = 0;
    }
}

static void reset_units(void) {
    reset_domains();
    for (u32 i = 0; i < VTD64_UNIT_MAX; i++) {
        if (units[i].regs) vm64_iounmap((void *)units[i].regs, 4096);
        for (u32 n = 0; n < VTD64_CONTEXT_MAX; n++) {
            if (units[i].contexts[n]) pmm_free_page(units[i].contexts[n]);
            units[i].contexts[n] = 0;
            units[i].context_bus[n] = 0;
        }
        if (units[i].root) pmm_free_page(units[i].root);
        if (units[i].qi_queue) pmm_free_page(units[i].qi_queue);
        units[i].regs = 0;
        units[i].root = 0;
        units[i].qi_queue = 0;
        units[i].qi_tail = 0;
        units[i].cap = 0;
        units[i].ecap = 0;
        units[i].segment = 0;
        units[i].flags = 0;
        units[i].gcmd = 0;
    }
    if (qi_status) pmm_free_page(qi_status);
    qi_status = 0;
    if (ir_table) pmm_free_page(ir_table);
    ir_table = 0;
    ir_enabled = 0;
    for (u32 i = 0; i < VTD64_IR_ENTRIES; i++) ir_used[i] = 0;
    unit_count = 0;
    host_width = 0;
    fault_count = 0;
    detected = 0;
    translation_enabled = 0;
}

static struct vtd64_domain *find_domain(u32 owner) {
    if (!owner) return 0;
    for (u32 i = 0; i < VTD64_DOMAIN_MAX; i++)
        if (domains[i].active && domains[i].owner == owner) return &domains[i];
    return 0;
}

static void qi_write_descriptor(struct vtd64_unit *unit, u64 low, u64 high) {
    u64 *queue = page(unit->qi_queue);
    queue[unit->qi_tail * 2] = low;
    queue[unit->qi_tail * 2 + 1] = high;
    unit->qi_tail = (unit->qi_tail + 1) % VTD64_QI_DESCRIPTORS;
}

// Interrupt remapping mandates queued invalidation, and register-based
// invalidation stops working once QI is enabled (VT-d 6.5.1). Every flush is a
// descriptor followed by a fenced wait descriptor whose status write we poll,
// so the caller sees the invalidation actually drained before returning.
static int qi_submit(struct vtd64_unit *unit, u64 invalidation, u64 high) {
    volatile u32 *status = (volatile u32 *)(uptr_t)qi_status;
    *status = 0;
    qi_write_descriptor(unit, invalidation, high);
    qi_write_descriptor(unit,
                        VTD64_QI_WAIT | VTD64_QI_WAIT_SW | VTD64_QI_WAIT_FN |
                        ((u64)VTD64_QI_STATUS << 32), qi_status);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    write64(unit->regs, VTD64_REG_IQT, (u64)unit->qi_tail * 16);
    for (u32 i = 0; i < VTD64_WAIT_MAX; i++)
        if (*status == VTD64_QI_STATUS) return 0;
    return -1;
}

static int unit_enable_qi(struct vtd64_unit *unit) {
    if (unit->gcmd & VTD64_GCMD_QIE) return 0;
    write64(unit->regs, VTD64_REG_IQT, 0);
    write64(unit->regs, VTD64_REG_IQA, unit->qi_queue);
    unit->qi_tail = 0;
    unit->gcmd |= VTD64_GCMD_QIE;
    write32(unit->regs, VTD64_REG_GCMD, unit->gcmd);
    return wait32(unit->regs, VTD64_REG_GSTS,
                  VTD64_GSTS_QIES, VTD64_GSTS_QIES);
}

static int invalidate_unit(struct vtd64_unit *unit) {
    if (qi_submit(unit, VTD64_QI_CC | VTD64_QI_GLOBAL, 0)) return -1;
    return qi_submit(unit,
                     VTD64_QI_IOTLB | VTD64_QI_GLOBAL | VTD64_QI_IOTLB_DRAIN, 0);
}

static int invalidate_mask(u32 mask) {
    flush_tables(mask);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (u32 i = 0; i < unit_count; i++)
        if ((mask & (1u << i)) && invalidate_unit(&units[i])) return -1;
    return 0;
}

// Drain stale interrupt remapping entries after a table write, the interrupt
// analogue of the IOTLB flush. In the IEC descriptor bit 4 is the granularity
// bit where 0 means global, the opposite polarity from the CC and IOTLB
// descriptors above, so global invalidation submits the type field alone.
static int invalidate_iec(void) {
    for (u32 i = 0; i < unit_count; i++)
        if (qi_submit(&units[i], VTD64_QI_IEC, 0)) return -1;
    return 0;
}

static int unit_set_root(struct vtd64_unit *unit) {
    write64(unit->regs, VTD64_REG_RTADDR, unit->root);
    write32(unit->regs, VTD64_REG_GCMD, unit->gcmd | VTD64_GCMD_SRTP);
    return wait32(unit->regs, VTD64_REG_GSTS,
                  VTD64_GSTS_RTPS, VTD64_GSTS_RTPS);
}

static int unit_enable(struct vtd64_unit *unit) {
    unit->gcmd |= VTD64_GCMD_TE;
    write32(unit->regs, VTD64_REG_GCMD, unit->gcmd);
    return wait32(unit->regs, VTD64_REG_GSTS,
                  VTD64_GSTS_TES, VTD64_GSTS_TES);
}

static int unit_disable(struct vtd64_unit *unit) {
    unit->gcmd &= ~VTD64_GCMD_TE;
    write32(unit->regs, VTD64_REG_GCMD, unit->gcmd);
    return wait32(unit->regs, VTD64_REG_GSTS, VTD64_GSTS_TES, 0);
}

static int unit_set_ir_root(struct vtd64_unit *unit) {
    write64(unit->regs, VTD64_REG_IRTA, ir_table | VTD64_IR_SIZE);
    write32(unit->regs, VTD64_REG_GCMD, unit->gcmd | VTD64_GCMD_SIRTP);
    return wait32(unit->regs, VTD64_REG_GSTS,
                  VTD64_GSTS_IRTPS, VTD64_GSTS_IRTPS);
}

static int unit_enable_ir(struct vtd64_unit *unit) {
    unit->gcmd |= VTD64_GCMD_IRE;
    write32(unit->regs, VTD64_REG_GCMD, unit->gcmd);
    return wait32(unit->regs, VTD64_REG_GSTS,
                  VTD64_GSTS_IRES, VTD64_GSTS_IRES);
}

// Queued invalidation must already be running: the interrupt entry cache is
// only reachable through QI, register-based invalidation cannot flush it. The
// table page itself is reserved at discovery so enabling does not perturb the
// page accounting the domain self-tests assert against.
static int enable_ir(void) {
    if (!ir_table) return -1;
    zero_page(ir_table);
    flush_page(ir_table);
    __asm__ volatile("mfence" : : : "memory");
    for (u32 i = 0; i < unit_count; i++)
        if (unit_set_ir_root(&units[i])) return -1;
    if (invalidate_iec()) return -1;
    for (u32 i = 0; i < unit_count; i++)
        if (unit_enable_ir(&units[i])) return -1;
    ir_enabled = 1;
    return 0;
}

static int unit_covers(u32 index, const struct pci_resource *pci) {
    const struct acpi_dmar_info *dmar = acpi64_dmar();
    if (!dmar || index >= dmar->unit_count) return 0;
    const struct acpi_dmar_unit *unit = &dmar->units[index];
    if (unit->segment != pci->segment) return 0;
    for (u32 i = 0; i < unit->scope_count; i++) {
        const struct acpi_dmar_scope *scope =
            &dmar->scopes[unit->scope_first + i];
        if (scope->type == 1 && scope->path_length == 1 &&
            scope->start_bus == pci->bus &&
            scope->device[0] == pci->device &&
            scope->function[0] == pci->function)
            return 2;
    }
    return unit->flags & 1 ? 1 : 0;
}

static int add_table_page(struct vtd64_domain *d, paddr_t physical) {
    if (!physical || d->table_page_count >= VTD64_TABLE_PAGE_MAX) return -1;
    d->table_pages[d->table_page_count++] = physical;
    zero_page(physical);
    return 0;
}

static u64 *domain_pte(struct vtd64_domain *d, u64 iova, int create) {
    if (!d || !d->root || iova >= (1ULL << 39)) return 0;
    u32 index[3] = {
        (u32)((iova >> 30) & 0x1FF),
        (u32)((iova >> 21) & 0x1FF),
        (u32)((iova >> 12) & 0x1FF),
    };
    paddr_t table = d->root;
    for (u32 level = 0; level < 2; level++) {
        u64 *entry = &page(table)[index[level]];
        if (!(*entry & VTD64_PAGE_READ)) {
            if (!create) return 0;
            paddr_t child = pmm_alloc_page();
            if (!child || add_table_page(d, child)) {
                if (child) pmm_free_page(child);
                return 0;
            }
            *entry = child | VTD64_PAGE_READ | VTD64_PAGE_WRITE;
        }
        table = *entry & ~0xFFFULL;
    }
    return &page(table)[index[2]];
}

static int context_slot(struct vtd64_unit *unit, u8 bus, int create) {
    for (u32 i = 0; i < VTD64_CONTEXT_MAX; i++)
        if (unit->contexts[i] && unit->context_bus[i] == bus) return (int)i;
    if (!create) return -1;
    for (u32 i = 0; i < VTD64_CONTEXT_MAX; i++) {
        if (unit->contexts[i]) continue;
        paddr_t context = pmm_alloc_page();
        if (!context) return -1;
        zero_page(context);
        unit->contexts[i] = context;
        unit->context_bus[i] = bus;
        page(unit->root)[(u32)bus * 2] = context | 1;
        return (int)i;
    }
    return -1;
}

static int install_context(struct vtd64_domain *d) {
    u32 devfn = ((u32)d->device << 3) | d->function;
    for (u32 i = 0; i < unit_count; i++) {
        if (!(d->unit_mask & (1u << i))) continue;
        struct vtd64_unit *unit = &units[i];
        int slot = context_slot(unit, d->bus, 1);
        if (slot < 0) return -1;
        u64 *entry = &page(unit->contexts[slot])[devfn * 2];
        if (entry[0] & 1) return -1;
        entry[1] = ((u64)d->did << 8) | 1;
        entry[0] = d->root | 1;
    }
    return 0;
}

static void clear_context(struct vtd64_domain *d) {
    u32 devfn = ((u32)d->device << 3) | d->function;
    for (u32 i = 0; i < unit_count; i++) {
        if (!(d->unit_mask & (1u << i))) continue;
        int slot = context_slot(&units[i], d->bus, 0);
        if (slot < 0) continue;
        u64 *entry = &page(units[i].contexts[slot])[devfn * 2];
        entry[0] = 0;
        entry[1] = 0;
    }
}

static void release_empty_contexts(struct vtd64_domain *d) {
    for (u32 i = 0; i < unit_count; i++) {
        if (!(d->unit_mask & (1u << i))) continue;
        int slot = context_slot(&units[i], d->bus, 0);
        if (slot < 0) continue;
        paddr_t context = units[i].contexts[slot];
        u64 *table = page(context);
        int used = 0;
        for (u32 n = 0; n < 512; n++) used |= table[n] != 0;
        if (used) continue;
        page(units[i].root)[(u32)d->bus * 2] = 0;
        units[i].contexts[slot] = 0;
        units[i].context_bus[slot] = 0;
        pmm_free_page(context);
    }
}

int vtd64_init(void) {
    reset_units();
    const struct acpi_dmar_info *dmar = acpi64_dmar();
    if (!dmar) return 0;
    if (!dmar->unit_count || dmar->unit_count > VTD64_UNIT_MAX) return -1;
    host_width = (u32)dmar->host_address_width + 1;
    qi_status = pmm_alloc_page();
    if (!qi_status) return -1;
    for (u32 i = 0; i < dmar->unit_count; i++) {
        const struct acpi_dmar_unit *src = &dmar->units[i];
        volatile u8 *regs = vm64_ioremap(
            src->register_base, 4096, VM64_CACHE_UC);
        if (!regs) {
            reset_units();
            return -1;
        }
        u32 version = reg32(regs, VTD64_REG_VERSION);
        u64 cap = reg64(regs, VTD64_REG_CAP);
        u64 ecap = reg64(regs, VTD64_REG_ECAP);
        u32 major = (version >> 4) & 0xF;
        u32 sagaw = (u32)((cap >> 8) & 0x1F);
        paddr_t root = pmm_alloc_page();
        paddr_t queue = pmm_alloc_page();
        // ECAP.QI (bit 1): queued invalidation is now required, not optional.
        if (!major || !(sagaw & 2) || !(ecap & 2) || cap == ~0ULL ||
            ecap == ~0ULL || !root || !queue) {
            if (root) pmm_free_page(root);
            if (queue) pmm_free_page(queue);
            vm64_iounmap((void *)regs, 4096);
            reset_units();
            return -1;
        }
        zero_page(root);
        units[i].regs = regs;
        units[i].root = root;
        units[i].qi_queue = queue;
        units[i].qi_tail = 0;
        units[i].cap = cap;
        units[i].ecap = ecap;
        units[i].segment = src->segment;
        units[i].flags = src->flags;
        unit_count++;
    }
    int ir_supported = unit_count > 0;
    for (u32 i = 0; i < unit_count; i++)
        if (!(units[i].ecap & VTD64_ECAP_IR)) ir_supported = 0;
    if (ir_supported) {
        ir_table = pmm_alloc_page();
        if (!ir_table) {
            reset_units();
            return -1;
        }
        zero_page(ir_table);
    }
    detected = 1;
    return 0;
}

int vtd64_present(void) {
    return detected;
}

u32 vtd64_unit_count(void) {
    return unit_count;
}

u32 vtd64_host_address_width(void) {
    return host_width;
}

int vtd64_translation_enabled(void) {
    return translation_enabled;
}

int vtd64_enable(void) {
    const struct acpi_dmar_info *dmar = acpi64_dmar();
    if (!detected || translation_enabled || !dmar || dmar->rmrr_count)
        return -1;
    int active = 0;
    for (u32 i = 0; i < VTD64_DOMAIN_MAX; i++) active |= domains[i].active != 0;
    if (!active) return -1;
    for (u32 i = 0; i < unit_count; i++)
        if (unit_enable_qi(&units[i])) return -1;
    flush_tables((1u << unit_count) - 1);
    for (u32 i = 0; i < unit_count; i++)
        if (unit_set_root(&units[i])) return -1;
    if (invalidate_mask((1u << unit_count) - 1)) return -1;
    // All or nothing: a unit left without TE would DMA untranslated, so any
    // failure rolls back the units already enabled and leaves translation_enabled
    // clear. Reporting a half-configured IOMMU as on is how a device behind the
    // un-enabled unit gets unconfined DMA while the supervisor, seeing the flag,
    // skips the enable and spawns the capsule anyway.
    for (u32 i = 0; i < unit_count; i++)
        if (unit_enable(&units[i])) {
            for (u32 j = 0; j < i; j++) unit_disable(&units[j]);
            return -1;
        }
    // Interrupt remapping wherever every unit offers it, brought up before the
    // global flag so a supported-but-broken IR unwinds translation too rather
    // than leaving the interrupt path unguarded while the IOMMU reports as on. A
    // device could otherwise forge an interrupt outside the entries bound to its
    // own source-id.
    int ir_required = unit_count > 0;
    for (u32 i = 0; i < unit_count; i++)
        if (!(units[i].ecap & VTD64_ECAP_IR)) ir_required = 0;
    if (ir_required && !ir_enabled && enable_ir()) {
        for (u32 i = 0; i < unit_count; i++) unit_disable(&units[i]);
        return -1;
    }
    translation_enabled = 1;
    return 0;
}

int vtd64_ir_active(void) {
    return ir_enabled;
}

// Reserve a run of consecutive remapping entries and program each to deliver a
// fixed, physically-addressed interrupt validated against source_id. A run lets
// multi-message MSI share one base handle while the device selects the member
// through the subhandle, so vectors are assigned base..base+count-1.
int vtd64_ir_allocate(u16 source_id, u8 vector, u8 destination,
                      int level, u16 count, u16 *handle) {
    if (!ir_enabled || !handle || !count || count > VTD64_IR_ENTRIES)
        return -1;
    for (u32 base = 0; base + count <= VTD64_IR_ENTRIES; base++) {
        int available = 1;
        for (u16 n = 0; n < count; n++)
            if (ir_used[base + n]) {
                available = 0;
                break;
            }
        if (!available) continue;
        for (u16 n = 0; n < count; n++) {
            u64 *irte = &page(ir_table)[(base + n) * 2];
            u64 low = VTD64_IRTE_PRESENT |
                      ((u64)(u8)(vector + n) << 16) |
                      ((u64)destination << 40);
            if (level) low |= VTD64_IRTE_TRIGGER_LEVEL;
            irte[0] = low;
            irte[1] = (u64)source_id | VTD64_IRTE_SVT_SID;
            ir_used[base + n] = 1;
        }
        flush_page(ir_table);
        __asm__ volatile("mfence" : : : "memory");
        if (invalidate_iec()) {
            for (u16 n = 0; n < count; n++) {
                u64 *irte = &page(ir_table)[(base + n) * 2];
                irte[0] = 0;
                irte[1] = 0;
                ir_used[base + n] = 0;
            }
            return -1;
        }
        *handle = (u16)base;
        return 0;
    }
    return -1;
}

int vtd64_ir_entry(u16 handle, u64 *low, u64 *high) {
    if (!ir_table || handle >= VTD64_IR_ENTRIES || !low || !high) return -1;
    u64 *irte = &page(ir_table)[(u32)handle * 2];
    *low = irte[0];
    *high = irte[1];
    return 0;
}

int vtd64_ir_release(u16 handle, u16 count) {
    if (!ir_enabled || !count || (u32)handle + count > VTD64_IR_ENTRIES)
        return -1;
    for (u16 n = 0; n < count; n++)
        if (!ir_used[handle + n]) return -1;
    for (u16 n = 0; n < count; n++) {
        u64 *irte = &page(ir_table)[(handle + n) * 2];
        irte[0] = 0;
        irte[1] = 0;
        ir_used[handle + n] = 0;
    }
    flush_page(ir_table);
    __asm__ volatile("mfence" : : : "memory");
    return invalidate_iec();
}

// Remappable MSI address carries the entry handle, not an APIC id: the format
// bit routes the request through the remapping table where destination and
// vector actually live. A multi-message group sets SHV so the device's message
// number becomes the subhandle added to this base handle.
void vtd64_ir_compose_msi(u16 handle, int multi, u32 *address, u32 *data) {
    u32 value = 0xFEE00000u | (1u << 3) |
                (((u32)handle & 0x7FFFu) << 5) |
                ((((u32)handle >> 15) & 1u) << 2);
    if (multi) value |= 1u << 4;
    if (address) *address = value;
    if (data) *data = 0;
}

void vtd64_ir_compose_ioapic(u16 handle, u8 vector, int level, int active_low,
                             int masked, u32 *low, u32 *high) {
    u32 value = vector | ((((u32)handle >> 15) & 1u) << 11);
    if (active_low) value |= 1u << 13;
    if (level) value |= 1u << 15;
    if (masked) value |= 1u << 16;
    if (low) *low = value;
    if (high) *high = (1u << 16) | (((u32)handle & 0x7FFFu) << 17);
}

// The IOAPIC has no config space to read a source-id from, so its remapping
// entries take the source reported by its DMAR device scope (type 3).
int vtd64_ir_ioapic_source_id(u16 *source_id) {
    const struct acpi_dmar_info *dmar = acpi64_dmar();
    if (!source_id || !dmar) return -1;
    for (u32 i = 0; i < dmar->scope_count; i++) {
        const struct acpi_dmar_scope *scope = &dmar->scopes[i];
        if (scope->type != 3 || !scope->path_length) continue;
        *source_id = ((u16)scope->start_bus << 8) |
                     ((u16)scope->device[0] << 3) | scope->function[0];
        return 0;
    }
    return -1;
}

u32 vtd64_fault_count(void) {
    return fault_count;
}

int vtd64_fault_decode(u32 unit, u64 low, u64 high,
                       struct vtd64_fault *fault) {
    if (!fault || unit >= unit_count || !(high & VTD64_FAULT_VALID)) return -1;
    u16 sid = (u16)high;
    u8 bus = (u8)(sid >> 8);
    u8 dev = (u8)((sid >> 3) & 31);
    u8 fn = (u8)(sid & 7);
    fault->address = low & ~0xFFFULL;
    fault->owner = 0;
    fault->segment = units[unit].segment;
    fault->source_id = sid;
    fault->reason = (u8)(high >> 32);
    fault->write = (high & VTD64_FAULT_WRITE) != 0;
    for (u32 i = 0; i < VTD64_DOMAIN_MAX; i++) {
        struct vtd64_domain *d = &domains[i];
        if (d->active && d->segment == fault->segment && d->bus == bus &&
            d->device == dev && d->function == fn) {
            fault->owner = d->owner;
            break;
        }
    }
    return 0;
}

int vtd64_fault_poll(struct vtd64_fault *fault) {
    if (!detected || !fault) return -1;
    for (u32 i = 0; i < unit_count; i++) {
        u32 status = reg32(units[i].regs, VTD64_REG_FSTS) & 0x7F;
        if (!status) continue;
        u32 offset = (u32)((units[i].cap >> 24) & 0x3FF) * 16;
        u32 records = (u32)((units[i].cap >> 40) & 0xFF) + 1;
        if (offset > 4096 - 16 || records > (4096 - offset) / 16)
            return -1;
        for (u32 n = 0; n < records; n++) {
            u64 low = reg64(units[i].regs, offset + n * 16);
            u64 high = reg64(units[i].regs, offset + n * 16 + 8);
            if (!(high & VTD64_FAULT_VALID)) continue;
            if (vtd64_fault_decode(i, low, high, fault)) return -1;
            write64(units[i].regs, offset + n * 16 + 8,
                    high | VTD64_FAULT_VALID);
            write32(units[i].regs, VTD64_REG_FSTS, status);
            fault_count++;
            return 1;
        }
        write32(units[i].regs, VTD64_REG_FSTS, status);
    }
    return 0;
}

static int vtd64_iommu_fault_poll(struct iommu_fault *fault) {
    if (!fault) return -1;
    struct vtd64_fault vtd_fault;
    int rc = vtd64_fault_poll(&vtd_fault);
    if (rc <= 0) return rc;
    fault->address = vtd_fault.address;
    fault->owner = vtd_fault.owner;
    fault->segment = vtd_fault.segment;
    fault->source_id = vtd_fault.source_id;
    fault->reason = vtd_fault.reason;
    fault->write = vtd_fault.write;
    return 1;
}

int vtd64_register_backend(void) {
    static const struct iommu_backend backend = {
        .name = "intel-vtd",
        .enable = vtd64_enable,
        .enabled = vtd64_translation_enabled,
        .domain_create = vtd64_domain_create,
        .domain_exists = vtd64_domain_exists,
        .domain_map = vtd64_domain_map,
        .domain_unmap = vtd64_domain_unmap,
        .domain_suspend = vtd64_domain_suspend,
        .domain_resume = vtd64_domain_resume,
        .domain_destroy = vtd64_domain_destroy,
        .fault_poll = vtd64_iommu_fault_poll,
    };
    return detected ? iommu_register(&backend) : -1;
}

int vtd64_domain_create(u32 owner, struct kernel_object *pci_object) {
    if (!detected || !owner || find_domain(owner)) return -1;
    const struct pci_resource *pci = pci_resource_get(pci_object);
    if (!pci) return -1;
    u32 mask = 0;
    u32 specific = 0;
    for (u32 i = 0; i < unit_count; i++) {
        int match = unit_covers(i, pci);
        if (match == 2) specific |= 1u << i;
        else if (match == 1) mask |= 1u << i;
    }
    if (specific) mask = specific;
    if (!mask) return -1;
    for (u32 i = 0; i < VTD64_DOMAIN_MAX; i++) {
        struct vtd64_domain *d = &domains[i];
        if (d->active) continue;
        paddr_t root = pmm_alloc_page();
        if (!root) return -1;
        d->owner = owner;
        d->did = (u16)(i + 1);
        d->segment = pci->segment;
        d->bus = pci->bus;
        d->device = pci->device;
        d->function = pci->function;
        d->unit_mask = mask;
        d->root = root;
        d->table_page_count = 0;
        d->next_iova = VTD64_IOVA_BASE;
        d->suspended = 0;
        d->active = 1;
        for (u32 n = 0; n < VTD64_MAPPING_MAX; n++)
            d->mappings[n].active = 0;
        if (add_table_page(d, root) || install_context(d)) {
            clear_context(d);
            if (!translation_enabled || !invalidate_mask(d->unit_mask)) {
                release_empty_contexts(d);
                for (u32 n = 0; n < d->table_page_count; n++)
                    pmm_free_page(d->table_pages[n]);
                d->active = 0;
            }
            return -1;
        }
        if (translation_enabled && invalidate_mask(d->unit_mask)) return -1;
        return 0;
    }
    return -1;
}

int vtd64_domain_map(u32 owner, paddr_t physical, u32 pages, u64 *iova) {
    struct vtd64_domain *d = find_domain(owner);
    if (!d || d->suspended || !iova || !physical || (physical & 0xFFF) ||
        !pages || pages > 256)
        return -1;
    u64 length = (u64)pages * 4096;
    if (length > ~0ULL - physical) return -1;
    u64 start = d->next_iova;
    u64 limit = VTD64_IOVA_BASE + VTD64_IOVA_STRIDE;
    if (start > limit || length > limit - start) return -1;
    struct vtd64_mapping *mapping = 0;
    for (u32 i = 0; i < VTD64_MAPPING_MAX; i++)
        if (!d->mappings[i].active) {
            mapping = &d->mappings[i];
            break;
        }
    if (!mapping) return -1;
    u32 done = 0;
    for (; done < pages; done++) {
        u64 *pte = domain_pte(d, start + (u64)done * 4096, 1);
        if (!pte || (*pte & VTD64_PAGE_READ)) break;
        *pte = (physical + (paddr_t)done * 4096) |
               VTD64_PAGE_READ | VTD64_PAGE_WRITE;
    }
    if (done != pages) {
        while (done) {
            done--;
            u64 *pte = domain_pte(d, start + (u64)done * 4096, 0);
            if (pte) *pte = 0;
        }
        return -1;
    }
    if (translation_enabled && invalidate_mask(d->unit_mask)) {
        for (u32 n = 0; n < pages; n++) {
            u64 *pte = domain_pte(d, start + (u64)n * 4096, 0);
            if (pte) *pte = 0;
        }
        invalidate_mask(d->unit_mask);
        return -1;
    }
    mapping->iova = start;
    mapping->physical = physical;
    mapping->pages = pages;
    mapping->active = 1;
    d->next_iova += length;
    *iova = start;
    return 0;
}

int vtd64_domain_unmap(u32 owner, u64 iova, u32 pages) {
    struct vtd64_domain *d = find_domain(owner);
    if (!d || !iova || (iova & 0xFFF) || !pages) return -1;
    for (u32 i = 0; i < VTD64_MAPPING_MAX; i++) {
        struct vtd64_mapping *m = &d->mappings[i];
        if (!m->active || m->iova != iova || m->pages != pages) continue;
        if (!d->suspended) {
            for (u32 n = 0; n < pages; n++) {
                u64 *pte = domain_pte(d, iova + (u64)n * 4096, 0);
                if (!pte || !(*pte & VTD64_PAGE_READ)) return -1;
            }
            for (u32 n = 0; n < pages; n++)
                *domain_pte(d, iova + (u64)n * 4096, 0) = 0;
        }
        int rc = translation_enabled && !d->suspended
            ? invalidate_mask(d->unit_mask) : 0;
        m->active = 0;
        return rc;
    }
    return -1;
}

int vtd64_domain_translate(u32 owner, u64 iova, paddr_t *physical) {
    struct vtd64_domain *d = find_domain(owner);
    if (!d || d->suspended || !physical) return -1;
    u64 *pte = domain_pte(d, iova & ~0xFFFULL, 0);
    if (!pte || !(*pte & VTD64_PAGE_READ)) return -1;
    *physical = (*pte & ~0xFFFULL) | (iova & 0xFFF);
    return 0;
}

int vtd64_domain_exists(u32 owner) {
    return find_domain(owner) != 0;
}

int vtd64_domain_suspend(u32 owner) {
    struct vtd64_domain *d = find_domain(owner);
    if (!d || d->suspended) return d && d->suspended ? 0 : -1;
    for (u32 i = 0; i < VTD64_MAPPING_MAX; i++) {
        struct vtd64_mapping *m = &d->mappings[i];
        if (!m->active) continue;
        for (u32 n = 0; n < m->pages; n++) {
            u64 *pte = domain_pte(d, m->iova + (u64)n * 4096, 0);
            if (!pte || !(*pte & VTD64_PAGE_READ)) return -1;
            *pte = 0;
        }
    }
    d->suspended = 1;
    return translation_enabled ? invalidate_mask(d->unit_mask) : 0;
}

int vtd64_domain_resume(u32 owner) {
    struct vtd64_domain *d = find_domain(owner);
    if (!d || !d->suspended) return d && !d->suspended ? 0 : -1;
    for (u32 i = 0; i < VTD64_MAPPING_MAX; i++) {
        struct vtd64_mapping *m = &d->mappings[i];
        if (!m->active) continue;
        for (u32 n = 0; n < m->pages; n++) {
            u64 *pte = domain_pte(d, m->iova + (u64)n * 4096, 0);
            if (!pte || (*pte & VTD64_PAGE_READ)) return -1;
            *pte = (m->physical + (paddr_t)n * 4096) |
                   VTD64_PAGE_READ | VTD64_PAGE_WRITE;
        }
    }
    if (translation_enabled && invalidate_mask(d->unit_mask)) {
        for (u32 i = 0; i < VTD64_MAPPING_MAX; i++) {
            struct vtd64_mapping *m = &d->mappings[i];
            if (!m->active) continue;
            for (u32 n = 0; n < m->pages; n++) {
                u64 *pte = domain_pte(d, m->iova + (u64)n * 4096, 0);
                if (pte) *pte = 0;
            }
        }
        invalidate_mask(d->unit_mask);
        return -1;
    }
    d->suspended = 0;
    return 0;
}

int vtd64_domain_destroy(u32 owner) {
    struct vtd64_domain *d = find_domain(owner);
    if (!d) return -1;
    clear_context(d);
    if (translation_enabled && invalidate_mask(d->unit_mask)) return -1;
    release_empty_contexts(d);
    for (u32 i = 0; i < d->table_page_count; i++)
        pmm_free_page(d->table_pages[i]);
    d->owner = 0;
    d->root = 0;
    d->table_page_count = 0;
    d->suspended = 0;
    d->active = 0;
    return 0;
}
