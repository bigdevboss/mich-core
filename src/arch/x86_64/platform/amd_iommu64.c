#include "amd_iommu64.h"
#include "acpi64.h"
#include "vm64.h"
#include "pmm.h"
#include "resource.h"
#include "iommu.h"

#define AMD_IOMMU_UNIT_MAX ACPI_MAX_IVHD_UNITS
#define AMD_IOMMU_DOMAIN_MAX 8
#define AMD_IOMMU_MAPPING_MAX 8
#define AMD_IOMMU_TABLE_PAGE_MAX 16
#define AMD_IOMMU_MMIO_SIZE 0x4000
#define AMD_IOMMU_DEVTAB_BASE 0x0000
#define AMD_IOMMU_CMDBUF_BASE 0x0008
#define AMD_IOMMU_EVENT_BASE 0x0010
#define AMD_IOMMU_CONTROL 0x0018
#define AMD_IOMMU_CMD_HEAD 0x2000
#define AMD_IOMMU_CMD_TAIL 0x2008
#define AMD_IOMMU_EVENT_HEAD 0x2010
#define AMD_IOMMU_EVENT_TAIL 0x2018
#define AMD_IOMMU_STATUS 0x2020
#define AMD_IOMMU_CONTROL_ENABLE (1ULL << 0)
#define AMD_IOMMU_CONTROL_EVENT_ENABLE (1ULL << 2)
#define AMD_IOMMU_CONTROL_COMMAND_ENABLE (1ULL << 12)
#define AMD_IOMMU_STATUS_EVENT_RUN (1ULL << 3)
#define AMD_IOMMU_STATUS_COMMAND_RUN (1ULL << 4)
#define AMD_IOMMU_CMD_COMPLETION_WAIT (1ULL << 60)
#define AMD_IOMMU_CMD_INVALIDATE_DTE (2ULL << 60)
#define AMD_IOMMU_CMD_INVALIDATE_ALL (8ULL << 60)
#define AMD_IOMMU_COMMAND_BYTES 16
#define AMD_IOMMU_COMMAND_SIZE 4096
#define AMD_IOMMU_DEVTAB_PAGES 16
#define AMD_IOMMU_DEVTAB_ENTRIES 2048
#define AMD_IOMMU_DTE_VALID 1ULL
#define AMD_IOMMU_DTE_TRANSLATION_VALID 2ULL
#define AMD_IOMMU_MODE_SHIFT 9
#define AMD_IOMMU_READ (1ULL << 61)
#define AMD_IOMMU_WRITE (1ULL << 62)
#define AMD_IOMMU_IOVA_BASE 0x00100000ULL
#define AMD_IOMMU_IOVA_SIZE 0x01000000ULL
#define AMD_IOMMU_BUFFER_LENGTH 8ULL
#define AMD_IOMMU_WAIT_MAX 1000000

struct amd_iommu64_unit {
    volatile u8 *regs;
    paddr_t device_table;
    paddr_t command_buffer;
    paddr_t event_log;
    u64 control;
    u64 feature_info;
    u16 device_id;
    u16 segment;
};

struct amd_iommu64_mapping {
    u64 iova;
    paddr_t physical;
    u32 pages;
    u32 active;
};

struct amd_iommu64_domain {
    u32 owner;
    u16 did;
    u16 segment;
    u16 device_id;
    u32 unit_mask;
    paddr_t root;
    paddr_t table_pages[AMD_IOMMU_TABLE_PAGE_MAX];
    u32 table_page_count;
    struct amd_iommu64_mapping mappings[AMD_IOMMU_MAPPING_MAX];
    u64 next_iova;
    u32 suspended;
    u32 active;
};

static struct amd_iommu64_unit units[AMD_IOMMU_UNIT_MAX];
static struct amd_iommu64_domain domains[AMD_IOMMU_DOMAIN_MAX];
static u32 unit_count;
static int detected;
static int tables_ready;

static u64 reg64(const volatile u8 *base, u32 offset) {
    return *(const volatile u64 *)(base + offset);
}

static void write64(volatile u8 *base, u32 offset, u64 value) {
    *(volatile u64 *)(base + offset) = value;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static int wait_status(struct amd_iommu64_unit *unit, u64 mask) {
    for (u32 i = 0; i < AMD_IOMMU_WAIT_MAX; i++)
        if ((reg64(unit->regs, AMD_IOMMU_STATUS) & mask) == mask) return 0;
    return -1;
}

static int wait_register(struct amd_iommu64_unit *unit,
                         u32 offset, u64 value) {
    for (u32 i = 0; i < AMD_IOMMU_WAIT_MAX; i++)
        if (reg64(unit->regs, offset) == value) return 0;
    return -1;
}

static u64 *page(paddr_t physical) {
    return (u64 *)(uptr_t)physical;
}

static void zero_page(paddr_t physical) {
    u64 *p = page(physical);
    for (u32 i = 0; i < 512; i++) p[i] = 0;
}

static void reset_domains(void) {
    for (u32 i = 0; i < AMD_IOMMU_DOMAIN_MAX; i++) {
        struct amd_iommu64_domain *d = &domains[i];
        if (d->active)
            for (u32 n = 0; n < d->table_page_count; n++)
                pmm_free_page(d->table_pages[n]);
        d->owner = 0;
        d->root = 0;
        d->table_page_count = 0;
        d->suspended = 0;
        d->active = 0;
        for (u32 n = 0; n < AMD_IOMMU_MAPPING_MAX; n++)
            d->mappings[n].active = 0;
    }
}

static void free_unit_tables(struct amd_iommu64_unit *unit) {
    if (unit->device_table)
        for (u32 i = 0; i < AMD_IOMMU_DEVTAB_PAGES; i++)
            pmm_free_page(unit->device_table + (paddr_t)i * 4096);
    if (unit->command_buffer) pmm_free_page(unit->command_buffer);
    if (unit->event_log) pmm_free_page(unit->event_log);
    unit->device_table = 0;
    unit->command_buffer = 0;
    unit->event_log = 0;
}

static void reset_units(void) {
    reset_domains();
    for (u32 i = 0; i < AMD_IOMMU_UNIT_MAX; i++) {
        if (units[i].regs && units[i].control) {
            write64(units[i].regs, AMD_IOMMU_CONTROL, 0);
            units[i].control = 0;
        }
        free_unit_tables(&units[i]);
        if (units[i].regs)
            vm64_iounmap((void *)units[i].regs, AMD_IOMMU_MMIO_SIZE);
        units[i].regs = 0;
        units[i].feature_info = 0;
        units[i].device_id = 0;
        units[i].segment = 0;
    }
    unit_count = 0;
    detected = 0;
    tables_ready = 0;
}

static int allocate_unit_tables(struct amd_iommu64_unit *unit) {
    unit->device_table = pmm_alloc_contiguous(
        AMD_IOMMU_DEVTAB_PAGES, ~(paddr_t)0);
    unit->command_buffer = pmm_alloc_page();
    unit->event_log = pmm_alloc_page();
    if (!unit->device_table || !unit->command_buffer || !unit->event_log) {
        free_unit_tables(unit);
        return -1;
    }
    u64 *table = (u64 *)(uptr_t)unit->device_table;
    for (u32 i = 0; i < AMD_IOMMU_DEVTAB_PAGES * 512; i++) table[i] = 0;
    return 0;
}

static int submit_invalidations(struct amd_iommu64_unit *unit, u16 device_id) {
    u64 tail = reg64(unit->regs, AMD_IOMMU_CMD_TAIL) &
               (AMD_IOMMU_COMMAND_SIZE - 1);
    u64 *buffer = (u64 *)(uptr_t)unit->command_buffer;
    u64 command[3][2] = {
        {AMD_IOMMU_CMD_INVALIDATE_DTE | device_id, 0},
        {AMD_IOMMU_CMD_INVALIDATE_ALL, 0},
        {AMD_IOMMU_CMD_COMPLETION_WAIT, 0},
    };
    for (u32 i = 0; i < 3; i++) {
        u32 slot = (u32)(tail / AMD_IOMMU_COMMAND_BYTES);
        buffer[slot * 2] = command[i][0];
        buffer[slot * 2 + 1] = command[i][1];
        tail = (tail + AMD_IOMMU_COMMAND_BYTES) &
               (AMD_IOMMU_COMMAND_SIZE - 1);
    }
    for (u32 i = 0; i < AMD_IOMMU_COMMAND_SIZE; i += 64)
        __asm__ volatile("clflush (%0)" : : "r"((u8 *)buffer + i) : "memory");
    __asm__ volatile("mfence" : : : "memory");
    write64(unit->regs, AMD_IOMMU_CMD_TAIL, tail);
    return wait_register(unit, AMD_IOMMU_CMD_HEAD, tail);
}

static int program_unit(struct amd_iommu64_unit *unit) {
    u64 devtab = unit->device_table | (AMD_IOMMU_DEVTAB_PAGES - 1);
    u64 command = unit->command_buffer |
                  (AMD_IOMMU_BUFFER_LENGTH << 56);
    u64 events = unit->event_log |
                 (AMD_IOMMU_BUFFER_LENGTH << 56);
    write64(unit->regs, AMD_IOMMU_DEVTAB_BASE, devtab);
    write64(unit->regs, AMD_IOMMU_CMDBUF_BASE, command);
    write64(unit->regs, AMD_IOMMU_EVENT_BASE, events);
    write64(unit->regs, AMD_IOMMU_CMD_HEAD, 0);
    write64(unit->regs, AMD_IOMMU_CMD_TAIL, 0);
    write64(unit->regs, AMD_IOMMU_EVENT_HEAD, 0);
    write64(unit->regs, AMD_IOMMU_EVENT_TAIL, 0);
    unit->control = AMD_IOMMU_CONTROL_ENABLE |
                    AMD_IOMMU_CONTROL_EVENT_ENABLE |
                    AMD_IOMMU_CONTROL_COMMAND_ENABLE;
    write64(unit->regs, AMD_IOMMU_CONTROL, unit->control);
    if (wait_status(unit, AMD_IOMMU_STATUS_COMMAND_RUN |
                          AMD_IOMMU_STATUS_EVENT_RUN))
        return -1;
    u64 *cmd = (u64 *)(uptr_t)unit->command_buffer;
    cmd[0] = AMD_IOMMU_CMD_COMPLETION_WAIT;
    cmd[1] = 0;
    __asm__ volatile("clflush (%0); mfence" : : "r"(cmd) : "memory");
    write64(unit->regs, AMD_IOMMU_CMD_TAIL, AMD_IOMMU_COMMAND_BYTES);
    return wait_register(unit, AMD_IOMMU_CMD_HEAD, AMD_IOMMU_COMMAND_BYTES);
}

int amd_iommu64_init(void) {
    reset_units();
    const struct acpi_ivrs_info *ivrs = acpi64_ivrs();
    if (!ivrs) return 0;
    if (!ivrs->unit_count || ivrs->unit_count > AMD_IOMMU_UNIT_MAX) return -1;
    for (u32 i = 0; i < ivrs->unit_count; i++) {
        const struct acpi_ivhd_unit *src = &ivrs->units[i];
        volatile u8 *regs = vm64_ioremap(
            src->register_base, AMD_IOMMU_MMIO_SIZE, VM64_CACHE_UC);
        if (!regs) {
            reset_units();
            return -1;
        }
        units[i].regs = regs;
        units[i].feature_info = src->feature_info;
        units[i].device_id = src->device_id;
        units[i].segment = src->segment;
        unit_count++;
    }
    detected = 1;
    return 0;
}

int amd_iommu64_prepare(void) {
    if (!detected || tables_ready) return -1;
    for (u32 i = 0; i < unit_count; i++) {
        if (allocate_unit_tables(&units[i]) || program_unit(&units[i])) {
            reset_units();
            return -1;
        }
    }
    tables_ready = 1;
    return 0;
}

static struct amd_iommu64_domain *find_domain(u32 owner) {
    for (u32 i = 0; i < AMD_IOMMU_DOMAIN_MAX; i++)
        if (domains[i].active && domains[i].owner == owner) return &domains[i];
    return 0;
}

static int add_table_page(struct amd_iommu64_domain *d, paddr_t physical) {
    if (!physical || d->table_page_count >= AMD_IOMMU_TABLE_PAGE_MAX) return -1;
    d->table_pages[d->table_page_count++] = physical;
    zero_page(physical);
    return 0;
}

static u64 *domain_pte(struct amd_iommu64_domain *d, u64 iova, int create) {
    u32 index[3] = {
        (u32)((iova >> 30) & 0x1FF),
        (u32)((iova >> 21) & 0x1FF),
        (u32)((iova >> 12) & 0x1FF),
    };
    paddr_t table = d->root;
    for (u32 level = 0; level < 2; level++) {
        u64 *entry = &page(table)[index[level]];
        if (!(*entry & AMD_IOMMU_DTE_VALID)) {
            if (!create) return 0;
            paddr_t child = pmm_alloc_page();
            if (!child || add_table_page(d, child)) {
                if (child) pmm_free_page(child);
                return 0;
            }
            *entry = child | AMD_IOMMU_DTE_VALID |
                     ((u64)(2 - level) << AMD_IOMMU_MODE_SHIFT) |
                     AMD_IOMMU_READ | AMD_IOMMU_WRITE;
        }
        table = *entry & 0x000FFFFFFFFFF000ULL;
    }
    return &page(table)[index[2]];
}

static int flush_domain(struct amd_iommu64_domain *d) {
    for (u32 n = 0; n < d->table_page_count; n++) {
        u8 *p = (u8 *)(uptr_t)d->table_pages[n];
        for (u32 i = 0; i < 4096; i += 64)
            __asm__ volatile("clflush (%0)" : : "r"(p + i) : "memory");
    }
    for (u32 i = 0; i < unit_count; i++) {
        if (!(d->unit_mask & (1u << i))) continue;
        u8 *entry = (u8 *)(uptr_t)units[i].device_table +
                    (u32)d->device_id * 32;
        __asm__ volatile("clflush (%0)" : : "r"(entry) : "memory");
    }
    __asm__ volatile("mfence" : : : "memory");
    for (u32 i = 0; i < unit_count; i++)
        if ((d->unit_mask & (1u << i)) &&
            submit_invalidations(&units[i], d->device_id))
            return -1;
    return 0;
}

static void set_dte(struct amd_iommu64_domain *d, int present) {
    for (u32 i = 0; i < unit_count; i++) {
        if (!(d->unit_mask & (1u << i))) continue;
        u64 *entry = (u64 *)((u8 *)(uptr_t)units[i].device_table +
                             (u32)d->device_id * 32);
        entry[0] = present ? d->root | AMD_IOMMU_DTE_VALID |
            AMD_IOMMU_DTE_TRANSLATION_VALID | (3ULL << AMD_IOMMU_MODE_SHIFT) |
            AMD_IOMMU_READ | AMD_IOMMU_WRITE : 0;
        entry[1] = present ? (u64)d->did << 32 : 0;
        entry[2] = 0;
        entry[3] = 0;
    }
}

static int domain_create(u32 owner, struct kernel_object *pci_object) {
    const struct pci_resource *pci = pci_resource_get(pci_object);
    if (!tables_ready || !owner || !pci || find_domain(owner)) return -1;
    u16 device_id = ((u16)pci->bus << 8) |
                    ((u16)pci->device << 3) | pci->function;
    if (device_id >= AMD_IOMMU_DEVTAB_ENTRIES) return -1;
    u32 mask = 0;
    for (u32 i = 0; i < unit_count; i++)
        if (units[i].segment == pci->segment) mask |= 1u << i;
    if (!mask) return -1;
    for (u32 i = 0; i < AMD_IOMMU_DOMAIN_MAX; i++) {
        struct amd_iommu64_domain *d = &domains[i];
        if (d->active) continue;
        paddr_t root = pmm_alloc_page();
        if (!root) return -1;
        d->owner = owner;
        d->did = (u16)(i + 1);
        d->segment = pci->segment;
        d->device_id = device_id;
        d->unit_mask = mask;
        d->root = root;
        d->table_page_count = 0;
        d->next_iova = AMD_IOMMU_IOVA_BASE;
        d->suspended = 0;
        d->active = 1;
        for (u32 n = 0; n < AMD_IOMMU_MAPPING_MAX; n++)
            d->mappings[n].active = 0;
        if (add_table_page(d, root)) return -1;
        set_dte(d, 1);
        if (flush_domain(d)) return -1;
        return 0;
    }
    return -1;
}

static int domain_map(u32 owner, paddr_t physical, u32 pages, u64 *iova) {
    struct amd_iommu64_domain *d = find_domain(owner);
    if (!d || d->suspended || !physical || (physical & 0xFFF) || !pages ||
        pages > 256 || !iova)
        return -1;
    u64 length = (u64)pages * 4096;
    if (length > AMD_IOMMU_IOVA_BASE + AMD_IOMMU_IOVA_SIZE - d->next_iova)
        return -1;
    struct amd_iommu64_mapping *m = 0;
    for (u32 i = 0; i < AMD_IOMMU_MAPPING_MAX; i++)
        if (!d->mappings[i].active) {
            m = &d->mappings[i];
            break;
        }
    if (!m) return -1;
    u64 start = d->next_iova;
    for (u32 i = 0; i < pages; i++) {
        u64 *pte = domain_pte(d, start + (u64)i * 4096, 1);
        if (!pte || (*pte & AMD_IOMMU_DTE_VALID)) return -1;
        *pte = (physical + (paddr_t)i * 4096) | AMD_IOMMU_DTE_VALID |
               AMD_IOMMU_READ | AMD_IOMMU_WRITE;
    }
    if (flush_domain(d)) return -1;
    m->iova = start;
    m->physical = physical;
    m->pages = pages;
    m->active = 1;
    d->next_iova += length;
    *iova = start;
    return 0;
}

static int domain_translate(u32 owner, u64 iova, paddr_t *physical) {
    struct amd_iommu64_domain *d = find_domain(owner);
    if (!d || d->suspended || !physical) return -1;
    u64 *pte = domain_pte(d, iova & ~0xFFFULL, 0);
    if (!pte || !(*pte & AMD_IOMMU_DTE_VALID)) return -1;
    *physical = (*pte & 0x000FFFFFFFFFF000ULL) | (iova & 0xFFF);
    return 0;
}

static int domain_unmap(u32 owner, u64 iova, u32 pages) {
    struct amd_iommu64_domain *d = find_domain(owner);
    if (!d) return -1;
    for (u32 i = 0; i < AMD_IOMMU_MAPPING_MAX; i++) {
        struct amd_iommu64_mapping *m = &d->mappings[i];
        if (!m->active || m->iova != iova || m->pages != pages) continue;
        if (!d->suspended) {
            for (u32 n = 0; n < pages; n++) {
                u64 *pte = domain_pte(d, iova + (u64)n * 4096, 0);
                if (!pte || !(*pte & AMD_IOMMU_DTE_VALID)) return -1;
                *pte = 0;
            }
            if (flush_domain(d)) return -1;
        }
        m->active = 0;
        return 0;
    }
    return -1;
}

static int domain_destroy(u32 owner) {
    struct amd_iommu64_domain *d = find_domain(owner);
    if (!d) return -1;
    set_dte(d, 0);
    if (flush_domain(d)) return -1;
    for (u32 i = 0; i < d->table_page_count; i++)
        pmm_free_page(d->table_pages[i]);
    d->owner = 0;
    d->root = 0;
    d->table_page_count = 0;
    d->active = 0;
    return 0;
}

static int domain_suspend(u32 owner) {
    struct amd_iommu64_domain *d = find_domain(owner);
    if (!d || d->suspended) return d && d->suspended ? 0 : -1;
    for (u32 i = 0; i < AMD_IOMMU_MAPPING_MAX; i++) {
        struct amd_iommu64_mapping *m = &d->mappings[i];
        if (!m->active) continue;
        for (u32 n = 0; n < m->pages; n++) {
            u64 *pte = domain_pte(d, m->iova + (u64)n * 4096, 0);
            if (!pte || !(*pte & AMD_IOMMU_DTE_VALID)) return -1;
            *pte = 0;
        }
    }
    d->suspended = 1;
    return flush_domain(d);
}

static int domain_resume(u32 owner) {
    struct amd_iommu64_domain *d = find_domain(owner);
    if (!d || !d->suspended) return d && !d->suspended ? 0 : -1;
    for (u32 i = 0; i < AMD_IOMMU_MAPPING_MAX; i++) {
        struct amd_iommu64_mapping *m = &d->mappings[i];
        if (!m->active) continue;
        for (u32 n = 0; n < m->pages; n++) {
            u64 *pte = domain_pte(d, m->iova + (u64)n * 4096, 0);
            if (!pte || (*pte & AMD_IOMMU_DTE_VALID)) return -1;
            *pte = (m->physical + (paddr_t)n * 4096) |
                   AMD_IOMMU_DTE_VALID | AMD_IOMMU_READ | AMD_IOMMU_WRITE;
        }
    }
    if (flush_domain(d)) return -1;
    d->suspended = 0;
    return 0;
}

static int amd_fault_poll(struct iommu_fault *fault) {
    if (!fault) return -1;
    for (u32 i = 0; i < unit_count; i++) {
        struct amd_iommu64_unit *unit = &units[i];
        u64 head = reg64(unit->regs, AMD_IOMMU_EVENT_HEAD) & 0xFF0;
        u64 tail = reg64(unit->regs, AMD_IOMMU_EVENT_TAIL) & 0xFF0;
        if (head == tail) continue;
        u64 *entry = (u64 *)((u8 *)(uptr_t)unit->event_log + head);
        u64 low = entry[0];
        u64 high = entry[1];
        u16 device_id = (u16)low;
        fault->address = high << 3;
        fault->owner = 0;
        fault->segment = unit->segment;
        fault->source_id = device_id;
        fault->reason = (u8)(low >> 48);
        fault->write = (low & (1ULL << 55)) != 0;
        for (u32 n = 0; n < AMD_IOMMU_DOMAIN_MAX; n++)
            if (domains[n].active && domains[n].segment == unit->segment &&
                domains[n].device_id == device_id)
                fault->owner = domains[n].owner;
        head = (head + 16) & 0xFF0;
        write64(unit->regs, AMD_IOMMU_EVENT_HEAD, head);
        return 1;
    }
    return 0;
}

static int backend_enable(void) {
    return tables_ready ? 0 : -1;
}

static int backend_enabled(void) {
    return tables_ready;
}

static int backend_exists(u32 owner) {
    return find_domain(owner) != 0;
}

int amd_iommu64_register_backend(void) {
    static const struct iommu_backend backend = {
        .name = "amd-vi",
        .enable = backend_enable,
        .enabled = backend_enabled,
        .domain_create = domain_create,
        .domain_exists = backend_exists,
        .domain_map = domain_map,
        .domain_unmap = domain_unmap,
        .domain_suspend = domain_suspend,
        .domain_resume = domain_resume,
        .domain_destroy = domain_destroy,
        .fault_poll = amd_fault_poll,
    };
    return tables_ready ? iommu_register(&backend) : -1;
}

int amd_iommu64_test_domain(struct kernel_object *pci) {
    u32 free_pages = pmm_free_pages();
    paddr_t physical = pmm_alloc_page();
    u32 owner = 0xA00D;
    u64 iova = 0;
    paddr_t translated = 0;
    int valid = physical && !domain_create(owner, pci) &&
        !domain_map(owner, physical, 1, &iova) &&
        !domain_translate(owner, iova + 37, &translated) &&
        translated == physical + 37 && !domain_suspend(owner) &&
        domain_translate(owner, iova, &translated) < 0 &&
        !domain_resume(owner) &&
        !domain_translate(owner, iova + 37, &translated) &&
        translated == physical + 37 && !domain_unmap(owner, iova, 1) &&
        domain_translate(owner, iova, &translated) < 0 &&
        !domain_destroy(owner);
    if (find_domain(owner)) domain_destroy(owner);
    if (physical) pmm_free_page(physical);
    return valid && pmm_free_pages() == free_pages ? 0 : -1;
}

int amd_iommu64_present(void) {
    return detected;
}

int amd_iommu64_tables_ready(void) {
    return tables_ready;
}

u32 amd_iommu64_unit_count(void) {
    return unit_count;
}
