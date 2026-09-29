#include "tests64.h"
#include "test_report.h"
#include "types.h"
#include "object.h"
#include "resource.h"
#include "event.h"
#include "endpoint.h"
#include "bridge.h"
#include "pmm.h"
#include "vm64.h"
#include "pci64.h"
#include "vtd64.h"
#include "iommu.h"
#include "vector64.h"
#include "msi64.h"
#include "msix64.h"
#include "platform64.h"
#include "runtime64.h"
#include "irq_group.h"
#include "serial64.h"

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11E8
#define EDU_DMA_SOURCE 0x80
#define EDU_DMA_DESTINATION 0x88
#define EDU_DMA_COUNT 0x90
#define EDU_DMA_COMMAND 0x98
#define EDU_DMA_BUFFER 0x40000ULL
#define EDU_DMA_START_TO_RAM 3ULL
#define EDU_FORBIDDEN_IOVA 0x200000ULL
#define EDU_FAULT_POLL_MAX 20000000

int test_vtd64_tables(struct kernel_object *pci) {
    if (!vtd64_present() || !pci) return -1;
    u32 free_pages = pmm_free_pages();
    paddr_t page = pmm_alloc_page();
    if (!page) return -1;
    u32 owner = 0xD00D;
    u64 iova = 0;
    paddr_t translated = 0;
    const struct pci_resource *p = pci_resource_get(pci);
    struct vtd64_fault fault;
    u16 sid = p ? ((u16)p->bus << 8) | ((u16)p->device << 3) | p->function : 0;
    u64 fault_high = (1ULL << 63) | (1ULL << 62) | (0x05ULL << 32) | sid;
    int valid = p && !vtd64_domain_create(owner, pci) &&
        !vtd64_domain_map(owner, page, 1, &iova) && iova != page &&
        !(iova & 0xFFF) && !vtd64_enable() &&
        vtd64_translation_enabled() && !vtd64_fault_count() &&
        !vtd64_fault_decode(0, iova, fault_high, &fault) &&
        fault.owner == owner && fault.source_id == sid &&
        fault.address == iova && fault.reason == 5 && fault.write &&
        !vtd64_domain_translate(owner, iova + 123, &translated) &&
        translated == page + 123 && !vtd64_domain_suspend(owner) &&
        vtd64_domain_translate(owner, iova, &translated) < 0 &&
        !vtd64_domain_resume(owner) &&
        !vtd64_domain_translate(owner, iova + 123, &translated) &&
        translated == page + 123 &&
        !vtd64_domain_unmap(owner, iova, 1) &&
        vtd64_domain_translate(owner, iova, &translated) < 0 &&
        !vtd64_domain_destroy(owner);
    if (vtd64_domain_destroy(owner) == 0) valid = 0;
    pmm_free_page(page);
    return valid && pmm_free_pages() == free_pages ? 0 : -1;
}

int test_iommu64_forbidden_dma(void) {
    struct kernel_object *pci = 0;
    for (u32 i = 0; i < pci64_count(); i++) {
        const struct pci_resource *p = pci_resource_get(pci64_object(i));
        if (p && p->vendor_id == EDU_VENDOR_ID && p->device_id == EDU_DEVICE_ID) {
            pci = pci64_object(i);
            break;
        }
    }
    if (!pci || !iommu_enabled()) return -1;
    u32 free_pages = pmm_free_pages();
    u32 objects = object_active_count();
    u32 owner = 0xD00E;
    struct kernel_object *bar = 0;
    paddr_t guard = 0;
    u64 guard_iova = 0;
    void *map = 0;
    int valid = 0;
    if (iommu_domain_create(owner, pci)) goto out;
    guard = pmm_alloc_page();
    if (!guard || iommu_domain_map(owner, guard, 1, &guard_iova)) goto out;
    *(u64 *)(uptr_t)guard = 0x4D494348494F4D4DULL;
    bar = pci64_bar_create(pci, 0);
    const struct mmio_resource *mmio = mmio_resource_get(bar);
    if (!bar || !mmio || pci64_set_command(pci, 4, 0)) goto out;
    map = vm64_ioremap(mmio->physical, mmio->length, VM64_CACHE_UC);
    if (!map) goto out;
    volatile u8 *regs = map;
    *(volatile u64 *)(regs + EDU_DMA_SOURCE) = EDU_DMA_BUFFER;
    *(volatile u64 *)(regs + EDU_DMA_DESTINATION) = EDU_FORBIDDEN_IOVA;
    *(volatile u64 *)(regs + EDU_DMA_COUNT) = 4;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *(volatile u64 *)(regs + EDU_DMA_COMMAND) = EDU_DMA_START_TO_RAM;
    struct iommu_fault fault;
    for (u32 i = 0; i < EDU_FAULT_POLL_MAX; i++) {
        int rc = iommu_fault_poll(&fault);
        if (rc < 0) break;
        if (!rc) {
            __asm__ volatile("pause");
            continue;
        }
        const struct pci_resource *p = pci_resource_get(pci);
        u16 sid = ((u16)p->bus << 8) | ((u16)p->device << 3) | p->function;
        valid = fault.owner == owner && fault.segment == p->segment &&
                fault.source_id == sid && fault.address == EDU_FORBIDDEN_IOVA &&
                guard_iova != EDU_FORBIDDEN_IOVA &&
                *(u64 *)(uptr_t)guard == 0x4D494348494F4D4DULL;
        break;
    }
out:
    pci64_quiesce(pci);
    if (map && bar) {
        const struct mmio_resource *mmio = mmio_resource_get(bar);
        if (!mmio || vm64_iounmap(map, mmio->length)) valid = 0;
    }
    if (bar) object_release(bar);
    if (guard_iova && iommu_domain_unmap(owner, guard_iova, 1)) valid = 0;
    if (guard) pmm_free_page(guard);
    if (iommu_domain_exists(owner) && iommu_domain_destroy(owner)) valid = 0;
    return valid && pmm_free_pages() == free_pages &&
           object_active_count() == objects ? 0 : -1;
}

int test_vtd64_interrupt_remapping(void) {
    if (!vtd64_ir_active()) return -1;
    u16 sid = ((u16)0x1D << 3);
    u8 vector = 0x71;
    u8 destination = 0x03;
    u16 handle;
    if (vtd64_ir_allocate(sid, vector, destination, 0, 1, &handle)) return -1;
    u64 low = 0;
    u64 high = 0;
    int valid = !vtd64_ir_entry(handle, &low, &high) && (low & 1) &&
                ((low >> 16) & 0xFF) == vector &&
                ((low >> 40) & 0xFF) == destination && !((low >> 4) & 1) &&
                (u16)high == sid && ((high >> 18) & 3) == 1;
    u32 address = 0;
    u32 data = 0;
    vtd64_ir_compose_msi(handle, 0, &address, &data);
    valid = valid && (address & 0xFFF00000u) == 0xFEE00000u &&
            (address & (1u << 3)) && !(address & (1u << 4)) &&
            ((address >> 5) & 0x7FFFu) == handle && !data;
    if (vtd64_ir_release(handle, 1)) valid = 0;
    if (!vtd64_ir_entry(handle, &low, &high) && (low || high)) valid = 0;
    u16 group;
    if (vtd64_ir_allocate(sid, 0x80, destination, 0, 2, &group)) return -1;
    u64 first_low = 0, first_high = 0, second_low = 0, second_high = 0;
    valid = valid && !vtd64_ir_entry(group, &first_low, &first_high) &&
            !vtd64_ir_entry((u16)(group + 1), &second_low, &second_high) &&
            ((first_low >> 16) & 0xFF) == 0x80 &&
            ((second_low >> 16) & 0xFF) == 0x81 &&
            ((first_high >> 18) & 3) == 1 && ((second_high >> 18) & 3) == 1;
    vtd64_ir_compose_msi(group, 1, &address, &data);
    valid = valid && (address & (1u << 4)) &&
            ((address >> 5) & 0x7FFFu) == group;
    if (vtd64_ir_release(group, 2)) valid = 0;
    return valid ? 0 : -1;
}

static int endpoint_test_notify(void *context, u32 source) {
    if (!context) return -1;
    *(u32 *)context = source;
    return 0;
}

static int test_irq_event(const struct test64_env *env) {
    struct kernel_object *irq = platform64_irq_object(15);
    struct kernel_object *event = event_create(EVENT_AUTO_RESET, 0);
    if (!irq || !event || !irq_resource_set_mask(irq, 0)) {
        if (event) object_release(event);
        return -1;
    }
    if (irq_resource_bind(irq, event)) {
        object_release(event);
        return -2;
    }
    if (irq_resource_set_mask(irq, 0)) {
        irq_resource_unbind(irq);
        object_release(event);
        return -3;
    }
    if (event_wait(event, env->owner_slot) != 1 ||
        env->owner->state != TASK_BLOCKED_EVENT) {
        irq_resource_set_mask(irq, 1);
        irq_resource_unbind(irq);
        object_release(event);
        return -4;
    }
    struct irq_resource *resource = irq_resource_get(irq);
    if (!resource) {
        irq_resource_set_mask(irq, 1);
        irq_resource_unbind(irq);
        object_release(event);
        return -5;
    }
    irq64_dispatch(&(struct exception_frame64){ .vector = resource->vector });
    resource = irq_resource_get(irq);
    int valid = env->owner->state == TASK_RUNNING && resource &&
                resource->masked;
    int unbound = irq_resource_unbind(irq) == 0;
    object_release(event);
    if (!valid || !unbound) return -6;
    u32 delivered_source = 0xFFFFFFFFu;
    struct kernel_object *endpoint =
        endpoint_create(endpoint_test_notify, &delivered_source);
    irq = platform64_irq_object(14);
    if (!irq || !endpoint) {
        if (endpoint) object_release(endpoint);
        return -7;
    }
    if (irq_resource_bind(irq, endpoint)) {
        object_release(endpoint);
        return -8;
    }
    if (irq_resource_set_mask(irq, 0)) {
        irq_resource_unbind(irq);
        object_release(endpoint);
        return -9;
    }
    resource = irq_resource_get(irq);
    if (!resource) {
        irq_resource_set_mask(irq, 1);
        irq_resource_unbind(irq);
        object_release(endpoint);
        return -10;
    }
    u32 expected_source = resource->source;
    irq64_dispatch(&(struct exception_frame64){ .vector = resource->vector });
    resource = irq_resource_get(irq);
    valid = resource && resource->masked &&
            delivered_source == expected_source;
    unbound = irq_resource_unbind(irq) == 0;
    object_release(endpoint);
    if (!valid || !unbound) return -11;
    struct kernel_object *bridge = bridge_endpoint_create();
    irq = platform64_irq_object(13);
    if (!irq || !bridge) {
        if (bridge) object_release(bridge);
        return -12;
    }
    if (irq_resource_bind(irq, bridge)) {
        object_release(bridge);
        return -13;
    }
    if (irq_resource_set_mask(irq, 0)) {
        irq_resource_unbind(irq);
        object_release(bridge);
        return -14;
    }
    if (bridge_endpoint_wait(bridge, env->owner_slot) != 1 ||
        env->owner->state != TASK_BLOCKED_EVENT) {
        irq_resource_set_mask(irq, 1);
        irq_resource_unbind(irq);
        object_release(bridge);
        return -15;
    }
    resource = irq_resource_get(irq);
    if (!resource) {
        irq_resource_set_mask(irq, 1);
        irq_resource_unbind(irq);
        object_release(bridge);
        return -16;
    }
    expected_source = resource->source;
    irq64_dispatch(&(struct exception_frame64){ .vector = resource->vector });
    struct bridge_notification notification;
    valid = env->owner->state == TASK_RUNNING &&
            bridge_endpoint_read(bridge, &notification) == 0 &&
            notification.source == expected_source;
    unbound = irq_resource_unbind(irq) == 0;
    object_release(bridge);
    return valid && unbound ? 0 : -17;
}

static int test_msi(void) {
    static u32 vector_owner;
    u32 available = vector64_available();
    u8 probe_group[4];
    if (vector64_allocate_group((uptr_t)&vector_owner, 3, probe_group) == 0 ||
        vector64_available() != available ||
        vector64_allocate_group((uptr_t)&vector_owner, 4, probe_group) ||
        probe_group[0] % 4 || probe_group[1] != probe_group[0] + 1 ||
        probe_group[2] != probe_group[0] + 2 ||
        probe_group[3] != probe_group[0] + 3 ||
        vector64_release_group(probe_group, 4, (uptr_t)&vector_owner) ||
        vector64_available() != available)
        return -1;
    int probe_vector = vector64_allocate((uptr_t)&vector_owner);
    if (probe_vector < 0 ||
        vector64_owner((u8)probe_vector) != (uptr_t)&vector_owner ||
        vector64_release((u8)probe_vector, (uptr_t)&vector_owner))
        return -1;
    for (u32 index = 0; index < pci64_count(); index++) {
        struct kernel_object *pci = pci64_object(index);
        const struct pci_resource *resource = pci_resource_get(pci);
        if (!resource || !(resource->capability_flags & PCI_CAP_MSI)) continue;
        u32 count = pci64_msi_max_vectors(pci) >= 2 ? 2 : 1;
        if (count > 2) return -1;
        struct kernel_object *irqs[IRQ_GROUP_MAX];
        struct kernel_object *events[IRQ_GROUP_MAX];
        for (u32 member = 0; member < IRQ_GROUP_MAX; member++) {
            irqs[member] = 0;
            events[member] = 0;
        }
        if (msi64_create_group(pci, count, irqs, 2)) return -1;
        struct kernel_object *duplicate = msi64_create(pci);
        int valid = duplicate == 0;
        if (duplicate) object_release(duplicate);
        for (u32 member = 0; member < count; member++) {
            events[member] = event_create(EVENT_AUTO_RESET, 0);
            if (!events[member] || irq_resource_bind(irqs[member],
                                                     events[member]))
                valid = 0;
        }
        for (u32 member = 0; member < count; member++)
            if (valid && irq_resource_set_mask(irqs[member], 0)) valid = 0;
        for (u32 member = 0; member < count; member++)
            if (irq_resource_set_mask(irqs[member], 1)) valid = 0;
        for (u32 member = 0; member < count; member++) {
            if (events[member] && irq_resource_unbind(irqs[member])) valid = 0;
            if (events[member]) object_release(events[member]);
            object_release(irqs[member]);
        }
        return valid && vector64_available() == available ? 0 : -1;
    }
    return 1;
}

static int test_msix(void) {
    for (u32 index = 0; index < pci64_count(); index++) {
        struct kernel_object *pci = pci64_object(index);
        const struct pci_resource *resource = pci_resource_get(pci);
        if (!resource || !(resource->capability_flags & PCI_CAP_MSIX)) continue;
        u32 available = vector64_available();
        struct kernel_object *table = pci64_msix_table_create(pci);
        const struct msix_table_resource *table_info =
            msix_table_resource_get(table);
        if (!table || !table_info) {
            if (table) object_release(table);
            return -1;
        }
        struct kernel_object *rollback[2] = {0, 0};
        if (msix64_create_group(table, table_info->entries - 1, 2,
                                rollback, 2) == 0 ||
            vector64_available() != available) {
            object_release(table);
            return -1;
        }
        if (table_info->entries >= 2) {
            struct kernel_object *blocker = msix64_create(table, 1);
            u32 blocked_available = vector64_available();
            if (!blocker || msix64_create_group(table, 0, 2, rollback, 2) == 0 ||
                vector64_available() != blocked_available) {
                if (blocker) object_release(blocker);
                object_release(table);
                return -1;
            }
            object_release(blocker);
            if (vector64_available() != available) {
                object_release(table);
                return -1;
            }
        }
        u32 count = table_info->entries >= 2 ? 2 : 1;
        if (count > 2) {
            object_release(table);
            return -1;
        }
        struct kernel_object *irqs[IRQ_GROUP_MAX];
        struct kernel_object *events[IRQ_GROUP_MAX];
        for (u32 member = 0; member < IRQ_GROUP_MAX; member++) {
            irqs[member] = 0;
            events[member] = 0;
        }
        if (msix64_create_group(table, 0, count, irqs, 2)) {
            for (u32 member = 0; member < count; member++)
                if (irqs[member]) object_release(irqs[member]);
            object_release(table);
            return -1;
        }
        struct kernel_object *duplicate = msix64_create(table, 0);
        struct kernel_object *second_table = pci64_msix_table_create(pci);
        struct kernel_object *cross_duplicate = second_table
            ? msix64_create(second_table, 0) : 0;
        int valid = !duplicate && second_table && !cross_duplicate;
        if (duplicate) object_release(duplicate);
        if (cross_duplicate) object_release(cross_duplicate);
        for (u32 member = 0; member < count; member++) {
            events[member] = event_create(EVENT_AUTO_RESET, 0);
            if (!events[member] || irq_resource_bind(irqs[member],
                                                     events[member]))
                valid = 0;
        }
        for (u32 member = 0; member < count; member++)
            if (valid && irq_resource_set_mask(irqs[member], 0)) valid = 0;
        for (u32 member = 0; member < count; member++)
            if (irq_resource_set_mask(irqs[member], 1)) valid = 0;
        for (u32 member = 0; member < count; member++) {
            if (events[member] && irq_resource_unbind(irqs[member])) valid = 0;
            if (events[member]) object_release(events[member]);
            object_release(irqs[member]);
        }
        if (second_table) object_release(second_table);
        object_release(table);
        return valid && vector64_available() == available ? 0 : -1;
    }
    return 1;
}

static int test_platform_mmio(const struct test64_env *env) {
    const struct mmio_resource *r = mmio_resource_get(env->platform_mmio);
    if (!r) return -1;
    void *map = vm64_ioremap(r->physical, r->length, VM64_CACHE_UC);
    if (!map) return -1;
    volatile u32 apic_id = ((volatile u32 *)map)[8];
    (void)apic_id;
    return vm64_iounmap(map, r->length);
}

int tests64_run_hardware(const struct test64_env *env,
                         int destructive, int *msi, int *msix) {
    if (!env || !msi || !msix) return -1;
    *msi = 1;
    *msix = 1;
    if (!destructive) return 0;
    *msi = test_msi();
    *msix = test_msix();
    if (test_report_record(TEST_ID_MSI, *msi < 0 ? *msi : 0) ||
        test_report_record(TEST_ID_MSIX, *msix < 0 ? *msix : 0))
        return -1;
    return 0;
}

int tests64_run_irq(const struct test64_env *env,
                    int msi, int msix) {
    if (!env) return -1;
    if (test_report_record(TEST_ID_IRQ, test_irq_event(env)) ||
        test_report_record(TEST_ID_IOREMAP, test_platform_mmio(env)))
        return -1;
    serial64_write("Mich test64: multi-vector IRQ groups pass\n");
    serial64_write("Mich test64: atomic vector rollback pass\n");
    serial64_write("Mich test64: MSI vector allocator pass\n");
    serial64_write("Mich test64: MSI programming API pass\n");
    if (!msi) {
        serial64_write("Mich test64: MSI IRQ object pass\n");
        serial64_write("Mich test64: MSI hardware programming pass\n");
    }
    serial64_write("Mich test64: MSI-X table object pass\n");
    serial64_write("Mich test64: MSI-X programming API pass\n");
    if (!msix)
        serial64_write("Mich test64: MSI-X hardware programming pass\n");
    serial64_write("Mich test64: IRQ event binding pass\n");
    serial64_write("Mich test64: safe IRQ unmask pass\n");
    serial64_write("Mich test64: IRQ endpoint binding pass\n");
    serial64_write("Mich test64: Driver Bridge endpoint pass\n");
    serial64_write("Mich test64: ioremap pass\n");
    return 0;
}
