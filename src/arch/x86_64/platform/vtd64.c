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
#define VTD64_REG_CCMD 0x28
#define VTD64_REG_FSTS 0x34
#define VTD64_GCMD_TE (1u << 31)
#define VTD64_GCMD_SRTP (1u << 30)
#define VTD64_GSTS_TES (1u << 31)
#define VTD64_GSTS_RTPS (1u << 30)
#define VTD64_CCMD_ICC (1ULL << 63)
#define VTD64_CCMD_GLOBAL (1ULL << 61)
#define VTD64_IOTLB_IVT (1ULL << 63)
#define VTD64_IOTLB_GLOBAL (1ULL << 60)
#define VTD64_FAULT_VALID (1ULL << 63)
#define VTD64_FAULT_WRITE (1ULL << 62)
#define VTD64_WAIT_MAX 1000000
#define VTD64_IOVA_BASE 0x00100000ULL
#define VTD64_IOVA_STRIDE 0x01000000ULL
#define VTD64_PAGE_READ 1ULL
#define VTD64_PAGE_WRITE 2ULL

struct vtd64_unit {
    volatile u8 *regs;
    paddr_t root;
    paddr_t contexts[VTD64_CONTEXT_MAX];
    u8 context_bus[VTD64_CONTEXT_MAX];
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
static u32 unit_count;
static u32 host_width;
static int detected;
static int translation_enabled;
static u32 fault_count;

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

static int wait64(volatile u8 *base, u32 offset, u64 mask, u64 value) {
    for (u32 i = 0; i < VTD64_WAIT_MAX; i++)
        if ((reg64(base, offset) & mask) == value) return 0;
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
        units[i].regs = 0;
        units[i].root = 0;
        units[i].cap = 0;
        units[i].ecap = 0;
        units[i].segment = 0;
        units[i].flags = 0;
        units[i].gcmd = 0;
    }
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

static int invalidate_unit(struct vtd64_unit *unit) {
    write64(unit->regs, VTD64_REG_CCMD,
            VTD64_CCMD_ICC | VTD64_CCMD_GLOBAL);
    if (wait64(unit->regs, VTD64_REG_CCMD, VTD64_CCMD_ICC, 0)) return -1;
    u32 offset = (u32)((unit->ecap >> 8) & 0x3FF) * 16 + 8;
    if (offset > 4096 - 8) return -1;
    write64(unit->regs, offset, VTD64_IOTLB_IVT | VTD64_IOTLB_GLOBAL);
    return wait64(unit->regs, offset, VTD64_IOTLB_IVT, 0);
}

static int invalidate_mask(u32 mask) {
    flush_tables(mask);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (u32 i = 0; i < unit_count; i++)
        if ((mask & (1u << i)) && invalidate_unit(&units[i])) return -1;
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
        if (!major || !(sagaw & 2) || cap == ~0ULL || ecap == ~0ULL || !root) {
            if (root) pmm_free_page(root);
            vm64_iounmap((void *)regs, 4096);
            reset_units();
            return -1;
        }
        zero_page(root);
        units[i].regs = regs;
        units[i].root = root;
        units[i].cap = cap;
        units[i].ecap = ecap;
        units[i].segment = src->segment;
        units[i].flags = src->flags;
        unit_count++;
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
    flush_tables((1u << unit_count) - 1);
    for (u32 i = 0; i < unit_count; i++)
        if (unit_set_root(&units[i])) return -1;
    if (invalidate_mask((1u << unit_count) - 1)) return -1;
    u32 enabled = 0;
    for (u32 i = 0; i < unit_count; i++) {
        if (unit_enable(&units[i])) {
            translation_enabled = enabled != 0;
            return -1;
        }
        enabled++;
    }
    translation_enabled = 1;
    return 0;
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
