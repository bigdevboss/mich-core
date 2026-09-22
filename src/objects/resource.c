#include "resource.h"
#include "pmm.h"
#include "event.h"
#include "endpoint.h"
#include "protos.h"

static struct mmio_resource mmio_resources[RESOURCE_MMIO_MAX];
static struct irq_resource irq_resources[RESOURCE_IRQ_MAX];
static struct dma_resource dma_resources[RESOURCE_DMA_MAX];
static struct pci_resource pci_resources[RESOURCE_PCI_MAX];
static struct msix_table_resource msix_resources[RESOURCE_MSIX_MAX];
static struct page_resource page_resources[RESOURCE_PAGE_MAX];
static struct sg_resource sg_resources[RESOURCE_SG_MAX];
static irq_mask_backend_fn irq_mask_backends[IRQ_CONTROLLER_MAX];
static irq_release_backend_fn irq_release_backends[IRQ_CONTROLLER_MAX];
static page_revoke_backend_fn page_revoke_backend;
static dma_iommu_unmap_fn dma_iommu_unmap;

static void mmio_destroy(struct kernel_object *object) {
    u64 value = object->value;
    if (!value || value > RESOURCE_MMIO_MAX) return;
    struct mmio_resource *resource = &mmio_resources[value - 1];
    resource->physical = 0;
    resource->length = 0;
    resource->cache_mode = 0;
    resource->active = 0;
}

static void irq_destroy(struct kernel_object *object) {
    u64 value = object->value;
    if (!value || value > RESOURCE_IRQ_MAX) return;
    struct irq_resource *resource = &irq_resources[value - 1];
    if (resource->controller < IRQ_CONTROLLER_MAX &&
        irq_mask_backends[resource->controller])
        irq_mask_backends[resource->controller](resource->source, 1);
    if (resource->binding) object_release(resource->binding);
    if (resource->controller < IRQ_CONTROLLER_MAX &&
        irq_release_backends[resource->controller])
        irq_release_backends[resource->controller](resource->source);
    resource->controller = 0;
    resource->source = 0;
    resource->vector = 0;
    resource->trigger = 0;
    resource->polarity = 0;
    resource->masked = 1;
    resource->binding = 0;
    resource->active = 0;
}

static void dma_destroy(struct kernel_object *object) {
    u64 value = object->value;
    if (!value || value > RESOURCE_DMA_MAX) return;
    struct dma_resource *resource = &dma_resources[value - 1];
    if (resource->active && resource->iommu_owner &&
        (!dma_iommu_unmap || dma_iommu_unmap(
            resource->iommu_owner, resource->bus_address, resource->pages)))
        panic_str("DMA IOMMU unmap");
    if (resource->active) {
        for (u32 page = 0; page < resource->pages; page++)
            pmm_free_page(resource->physical + (paddr_t)page * 4096);
    }
    resource->physical = 0;
    resource->bus_address = 0;
    resource->iommu_owner = 0;
    resource->pages = 0;
    resource->address_mask = 0;
    resource->active = 0;
}

static void msix_destroy(struct kernel_object *object) {
    u64 value = object->value;
    if (!value || value > RESOURCE_MSIX_MAX) return;
    struct msix_table_resource *resource = &msix_resources[value - 1];
    if (resource->pci) object_release(resource->pci);
    resource->pci = 0;
    resource->table_bir = 0;
    resource->pba_bir = 0;
    resource->table_offset = 0;
    resource->pba_offset = 0;
    resource->entries = 0;
    resource->active = 0;
}

static void page_destroy(struct kernel_object *object) {
    u64 value = object->value;
    if (!value || value > RESOURCE_PAGE_MAX) return;
    struct page_resource *resource = &page_resources[value - 1];
    if (!resource->active) return;
    if (resource->pin_count || resource->map_count)
        panic_str("page resource reference invariant");
    for (u32 page = 0; page < resource->pages; page++) {
        if (resource->physical[page]) pmm_free_page(resource->physical[page]);
        resource->physical[page] = 0;
    }
    resource->pages = 0;
    resource->pin_count = 0;
    resource->map_count = 0;
    resource->revoked = 0;
    resource->shared = 0;
    resource->active = 0;
}

static void sg_destroy(struct kernel_object *object) {
    u64 value = object->value;
    if (!value || value > RESOURCE_SG_MAX) return;
    struct sg_resource *resource = &sg_resources[value - 1];
    if (!resource->active) return;
    for (u32 index = 0; index < resource->entry_count; index++) {
        if (resource->entries[index].page)
            page_resource_unpin(resource->entries[index].page);
        resource->entries[index].page = 0;
        resource->entries[index].physical = 0;
        resource->entries[index].page_index = 0;
        resource->entries[index].offset = 0;
        resource->entries[index].length = 0;
    }
    resource->entry_count = 0;
    resource->total_length = 0;
    resource->revoked = 0;
    resource->active = 0;
}

static void pci_destroy(struct kernel_object *object) {
    u64 value = object->value;
    if (!value || value > RESOURCE_PCI_MAX) return;
    struct pci_resource *resource = &pci_resources[value - 1];
    for (u32 bar = 0; bar < 6; bar++) {
        resource->bars[bar].address = 0;
        resource->bars[bar].length = 0;
        resource->bars[bar].flags = 0;
    }
    resource->active = 0;
}

void resource_init(void) {
    page_revoke_backend = 0;
    dma_iommu_unmap = 0;
    for (u32 index = 0; index < IRQ_CONTROLLER_MAX; index++) {
        irq_mask_backends[index] = 0;
        irq_release_backends[index] = 0;
    }
    for (u32 index = 0; index < RESOURCE_MMIO_MAX; index++)
        mmio_resources[index].active = 0;
    for (u32 index = 0; index < RESOURCE_IRQ_MAX; index++)
        irq_resources[index].active = 0;
    for (u32 index = 0; index < RESOURCE_DMA_MAX; index++)
        dma_resources[index].active = 0;
    for (u32 index = 0; index < RESOURCE_PCI_MAX; index++)
        pci_resources[index].active = 0;
    for (u32 index = 0; index < RESOURCE_MSIX_MAX; index++)
        msix_resources[index].active = 0;
    for (u32 index = 0; index < RESOURCE_PAGE_MAX; index++)
        page_resources[index].active = 0;
    for (u32 index = 0; index < RESOURCE_SG_MAX; index++)
        sg_resources[index].active = 0;
}

int irq_resource_set_backend(u32 controller, irq_mask_backend_fn mask,
                             irq_release_backend_fn release) {
    if (controller >= IRQ_CONTROLLER_MAX || !mask) return -1;
    irq_mask_backends[controller] = mask;
    irq_release_backends[controller] = release;
    return 0;
}

struct kernel_object *mmio_resource_create(paddr_t physical, usize_t length,
                                            u32 cache_mode) {
    if (!length || (physical & 0xFFF) || (length & 0xFFF) ||
        cache_mode > MMIO_CACHE_WB || length > ~(paddr_t)0 - physical ||
        pmm_range_is_ram(physical, length))
        return 0;
    for (u32 index = 0; index < RESOURCE_MMIO_MAX; index++) {
        struct mmio_resource *resource = &mmio_resources[index];
        if (resource->active) continue;
        resource->physical = physical;
        resource->length = length;
        resource->cache_mode = cache_mode;
        resource->active = 1;
        struct kernel_object *object =
            object_create(KOBJECT_MMIO, index + 1, mmio_destroy);
        if (!object) resource->active = 0;
        return object;
    }
    return 0;
}

const struct mmio_resource *mmio_resource_get(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_MMIO ||
        !object->value || object->value > RESOURCE_MMIO_MAX)
        return 0;
    const struct mmio_resource *resource = &mmio_resources[object->value - 1];
    return resource->active ? resource : 0;
}

struct kernel_object *irq_resource_create_kind(u32 controller, u32 source,
                                                u32 vector, u32 trigger,
                                                u32 polarity) {
    if (controller >= IRQ_CONTROLLER_MAX || vector < 32 || vector > 255 ||
        trigger > IRQ_TRIGGER_LEVEL || polarity > IRQ_POLARITY_LOW)
        return 0;
    for (u32 index = 0; index < RESOURCE_IRQ_MAX; index++) {
        struct irq_resource *resource = &irq_resources[index];
        if (resource->active) continue;
        resource->controller = controller;
        resource->source = source;
        resource->vector = vector;
        resource->trigger = trigger;
        resource->polarity = polarity;
        resource->masked = 1;
        resource->binding = 0;
        resource->active = 1;
        struct kernel_object *object =
            object_create(KOBJECT_IRQ, index + 1, irq_destroy);
        if (!object) resource->active = 0;
        return object;
    }
    return 0;
}

struct kernel_object *irq_resource_create(u32 source, u32 vector,
                                           u32 trigger, u32 polarity) {
    return irq_resource_create_kind(IRQ_CONTROLLER_IOAPIC, source, vector,
                                    trigger, polarity);
}

struct irq_resource *irq_resource_get(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_IRQ ||
        !object->value || object->value > RESOURCE_IRQ_MAX)
        return 0;
    struct irq_resource *resource = &irq_resources[object->value - 1];
    return resource->active ? resource : 0;
}

int irq_resource_bind(struct kernel_object *irq,
                      struct kernel_object *target) {
    struct irq_resource *resource = irq_resource_get(irq);
    if (!resource || !target || !target->active || resource->binding ||
        !resource->masked ||
        (target->type != KOBJECT_EVENT && target->type != KOBJECT_ENDPOINT))
        return -1;
    if (object_retain(target)) return -1;
    resource->binding = target;
    return 0;
}

int irq_resource_unbind(struct kernel_object *irq) {
    struct irq_resource *resource = irq_resource_get(irq);
    if (!resource || !resource->binding) return -1;
    if (!resource->masked && irq_resource_set_mask(irq, 1)) return -1;
    struct kernel_object *binding = resource->binding;
    resource->binding = 0;
    object_release(binding);
    return 0;
}

int irq_resource_set_mask(struct kernel_object *irq, int masked) {
    struct irq_resource *resource = irq_resource_get(irq);
    if (!resource || (!masked && !resource->binding) ||
        resource->controller >= IRQ_CONTROLLER_MAX ||
        !irq_mask_backends[resource->controller])
        return -1;
    if (irq_mask_backends[resource->controller](resource->source,
                                                masked != 0))
        return -1;
    resource->masked = masked != 0;
    return 0;
}

int irq_resource_signal(struct kernel_object *irq) {
    struct irq_resource *resource = irq_resource_get(irq);
    if (!resource || resource->masked || !resource->binding) return -1;
    if (resource->binding->type == KOBJECT_EVENT)
        return event_signal(resource->binding);
    if (resource->binding->type == KOBJECT_ENDPOINT)
        return endpoint_signal(resource->binding, resource->source);
    return -1;
}

struct kernel_object *dma_resource_allocate(u32 pages, paddr_t address_mask) {
    if (!pages || pages > 256 || address_mask < 0xFFFFF) return 0;
    for (u32 index = 0; index < RESOURCE_DMA_MAX; index++) {
        struct dma_resource *resource = &dma_resources[index];
        if (resource->active) continue;
        paddr_t physical = pmm_alloc_contiguous(pages, address_mask);
        if (!physical) return 0;
#if __SIZEOF_POINTER__ == 8
        u8 *memory = (u8 *)(uptr_t)physical;
        for (usize_t byte = 0; byte < (usize_t)pages * 4096; byte++)
            memory[byte] = 0;
#else
        if (physical >= 0x1000000) {
            for (u32 page = 0; page < pages; page++)
                pmm_free_page(physical + page * 4096);
            return 0;
        }
        u8 *memory = (u8 *)(uptr_t)physical;
        for (usize_t byte = 0; byte < (usize_t)pages * 4096; byte++)
            memory[byte] = 0;
#endif
        resource->physical = physical;
        resource->bus_address = physical;
        resource->iommu_owner = 0;
        resource->pages = pages;
        resource->address_mask = address_mask;
        resource->active = 1;
        struct kernel_object *object =
            object_create(KOBJECT_DMA, index + 1, dma_destroy);
        if (!object) {
            for (u32 page = 0; page < pages; page++)
                pmm_free_page(physical + (paddr_t)page * 4096);
            resource->active = 0;
        }
        return object;
    }
    return 0;
}

const struct dma_resource *dma_resource_get(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_DMA ||
        !object->value || object->value > RESOURCE_DMA_MAX)
        return 0;
    const struct dma_resource *resource = &dma_resources[object->value - 1];
    return resource->active ? resource : 0;
}

int dma_resource_set_iommu_backend(dma_iommu_unmap_fn unmap) {
    if (!unmap || dma_iommu_unmap) return -1;
    dma_iommu_unmap = unmap;
    return 0;
}

int dma_resource_bind_iommu(struct kernel_object *object, u32 owner,
                            u64 address) {
    if (!object || !object->active || object->type != KOBJECT_DMA ||
        !object->value || object->value > RESOURCE_DMA_MAX || !owner ||
        !address || (address & 0xFFF) || !dma_iommu_unmap)
        return -1;
    struct dma_resource *resource = &dma_resources[object->value - 1];
    if (!resource->active || resource->iommu_owner) return -1;
    resource->bus_address = address;
    resource->iommu_owner = owner;
    return 0;
}

struct kernel_object *pci_resource_create(const struct pci_resource *description) {
    if (!description || description->device > 31 || description->function > 7 ||
        description->vendor_id == 0xFFFF)
        return 0;
    for (u32 index = 0; index < RESOURCE_PCI_MAX; index++) {
        struct pci_resource *resource = &pci_resources[index];
        if (resource->active) continue;
        *resource = *description;
        resource->active = 1;
        struct kernel_object *object =
            object_create(KOBJECT_PCI, index + 1, pci_destroy);
        if (!object) resource->active = 0;
        return object;
    }
    return 0;
}

const struct pci_resource *pci_resource_get(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_PCI ||
        !object->value || object->value > RESOURCE_PCI_MAX)
        return 0;
    const struct pci_resource *resource = &pci_resources[object->value - 1];
    return resource->active ? resource : 0;
}


struct kernel_object *msix_table_resource_create(struct kernel_object *pci,
                                                  u8 table_bir,
                                                  u32 table_offset,
                                                  u8 pba_bir,
                                                  u32 pba_offset,
                                                  u32 entries) {
    if (!pci || !pci_resource_get(pci) || table_bir >= 6 || pba_bir >= 6 ||
        !entries || entries > 2048)
        return 0;
    for (u32 index = 0; index < RESOURCE_MSIX_MAX; index++) {
        struct msix_table_resource *resource = &msix_resources[index];
        if (resource->active) continue;
        if (object_retain(pci)) return 0;
        resource->pci = pci;
        resource->table_bir = table_bir;
        resource->pba_bir = pba_bir;
        resource->table_offset = table_offset;
        resource->pba_offset = pba_offset;
        resource->entries = entries;
        resource->active = 1;
        struct kernel_object *object =
            object_create(KOBJECT_MSIX_TABLE, index + 1, msix_destroy);
        if (!object) {
            resource->active = 0;
            resource->pci = 0;
            object_release(pci);
        }
        return object;
    }
    return 0;
}

const struct msix_table_resource *msix_table_resource_get(
    const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_MSIX_TABLE ||
        !object->value || object->value > RESOURCE_MSIX_MAX)
        return 0;
    const struct msix_table_resource *resource =
        &msix_resources[object->value - 1];
    return resource->active ? resource : 0;
}

u32 resource_active_count(u32 object_type) {
    u32 count = 0;
    if (object_type == KOBJECT_MMIO)
        for (u32 index = 0; index < RESOURCE_MMIO_MAX; index++)
            if (mmio_resources[index].active) count++;
    if (object_type == KOBJECT_IRQ)
        for (u32 index = 0; index < RESOURCE_IRQ_MAX; index++)
            if (irq_resources[index].active) count++;
    if (object_type == KOBJECT_DMA)
        for (u32 index = 0; index < RESOURCE_DMA_MAX; index++)
            if (dma_resources[index].active) count++;
    if (object_type == KOBJECT_PCI)
        for (u32 index = 0; index < RESOURCE_PCI_MAX; index++)
            if (pci_resources[index].active) count++;
    if (object_type == KOBJECT_MSIX_TABLE)
        for (u32 index = 0; index < RESOURCE_MSIX_MAX; index++)
            if (msix_resources[index].active) count++;
    if (object_type == KOBJECT_PAGE || object_type == KOBJECT_SHARED_MEMORY)
        for (u32 index = 0; index < RESOURCE_PAGE_MAX; index++)
            if (page_resources[index].active &&
                ((object_type == KOBJECT_PAGE && !page_resources[index].shared) ||
                 (object_type == KOBJECT_SHARED_MEMORY &&
                  page_resources[index].shared)))
                count++;
    if (object_type == KOBJECT_SG_LIST)
        for (u32 index = 0; index < RESOURCE_SG_MAX; index++)
            if (sg_resources[index].active) count++;
    return count;
}

int page_resource_set_revoke_backend(page_revoke_backend_fn revoke) {
    if (!revoke) return -1;
    page_revoke_backend = revoke;
    return 0;
}

static struct kernel_object *page_resource_allocate(u32 pages, int shared) {
    if (!pages || pages > RESOURCE_PAGE_PAGES_MAX) return 0;
    for (u32 index = 0; index < RESOURCE_PAGE_MAX; index++) {
        struct page_resource *resource = &page_resources[index];
        if (resource->active) continue;
        for (u32 page = 0; page < RESOURCE_PAGE_PAGES_MAX; page++)
            resource->physical[page] = 0;
        u32 allocated = 0;
        while (allocated < pages) {
            paddr_t physical = pmm_alloc_page();
            if (!physical) break;
            resource->physical[allocated++] = physical;
        }
        if (allocated != pages) {
            while (allocated) pmm_free_page(resource->physical[--allocated]);
            return 0;
        }
        resource->pages = pages;
        resource->pin_count = 0;
        resource->map_count = 0;
        resource->revoked = 0;
        resource->shared = shared != 0;
        resource->active = 1;
        struct kernel_object *object = object_create(
            shared ? KOBJECT_SHARED_MEMORY : KOBJECT_PAGE,
            index + 1, page_destroy);
        if (!object) {
            for (u32 page = 0; page < pages; page++)
                pmm_free_page(resource->physical[page]);
            resource->active = 0;
            resource->pages = 0;
        }
        return object;
    }
    return 0;
}

struct kernel_object *page_resource_create(void) {
    return page_resource_allocate(1, 0);
}

struct kernel_object *shared_memory_resource_create(u32 pages) {
    return page_resource_allocate(pages, 1);
}

struct page_resource *page_resource_get(const struct kernel_object *object) {
    if (!object || !object->active ||
        (object->type != KOBJECT_PAGE &&
         object->type != KOBJECT_SHARED_MEMORY) ||
        !object->value || object->value > RESOURCE_PAGE_MAX)
        return 0;
    struct page_resource *resource = &page_resources[object->value - 1];
    return resource->active ? resource : 0;
}

int page_resource_pin(struct kernel_object *object) {
    struct page_resource *resource = page_resource_get(object);
    if (!resource || resource->revoked || resource->pin_count == 0xFFFFFFFFu ||
        object_retain(object))
        return -1;
    resource->pin_count++;
    return 0;
}

int page_resource_unpin(struct kernel_object *object) {
    struct page_resource *resource = page_resource_get(object);
    if (!resource || !resource->pin_count) return -1;
    resource->pin_count--;
    object_release(object);
    return 0;
}

int page_resource_revoke(struct kernel_object *object) {
    struct page_resource *resource = page_resource_get(object);
    if (!resource || resource->revoked) return -1;
    if (page_revoke_backend && page_revoke_backend(object)) return -1;
    // Set revoked only after the backend succeeds so a failed
    // revoke remains retryable from the same caller.
    resource->revoked = 1;
    return resource->map_count || resource->pin_count ? -1 : 0;
}

int page_resource_grow(struct kernel_object *object, u32 pages) {
    struct page_resource *resource = page_resource_get(object);
    if (!resource || resource->revoked || pages <= resource->pages ||
        pages > RESOURCE_PAGE_PAGES_MAX)
        return -1;
    u32 grown = resource->pages;
    while (grown < pages) {
        paddr_t physical = pmm_alloc_page();
        if (!physical) break;
        resource->physical[grown++] = physical;
    }
    if (grown != pages) {
        while (grown > resource->pages)
            pmm_free_page(resource->physical[--grown]);
        return -1;
    }
    // pmm recycles frames; zero on attach so a hole never leaks prior data.
    for (u32 page = resource->pages; page < pages; page++) {
        u8 *bytes = (u8 *)(uptr_t)resource->physical[page];
        for (u32 index = 0; index < 4096; index++) bytes[index] = 0;
    }
    resource->pages = pages;
    return 0;
}

int page_resource_trim(struct kernel_object *object, u32 pages) {
    struct page_resource *resource = page_resource_get(object);
    if (!resource || resource->revoked || pages >= resource->pages ||
        resource->map_count || resource->pin_count)
        return -1;
    for (u32 page = pages; page < resource->pages; page++) {
        pmm_free_page(resource->physical[page]);
        resource->physical[page] = 0;
    }
    resource->pages = pages;
    return 0;
}

int page_resource_mapping_open(struct kernel_object *object) {
    struct page_resource *resource = page_resource_get(object);
    if (!resource || resource->revoked ||
        resource->map_count == 0xFFFFFFFFu)
        return -1;
    resource->map_count++;
    return 0;
}

void page_resource_mapping_close(struct kernel_object *object) {
    struct page_resource *resource = page_resource_get(object);
    if (resource && resource->map_count) resource->map_count--;
}

struct kernel_object *sg_resource_create(struct kernel_object **pages,
                                          const u32 *page_indices,
                                          const u32 *offsets,
                                          const u32 *lengths,
                                          u32 entry_count) {
    if (!pages || !page_indices || !offsets || !lengths || !entry_count ||
        entry_count > RESOURCE_SG_ENTRY_MAX)
        return 0;
    u32 total = 0;
    for (u32 index = 0; index < entry_count; index++) {
        struct page_resource *page = page_resource_get(pages[index]);
        if (!page || page->revoked || page_indices[index] >= page->pages ||
            offsets[index] >= 4096 || !lengths[index] ||
            lengths[index] > 4096 - offsets[index] ||
            lengths[index] > 0xFFFFFFFFu - total)
            return 0;
        total += lengths[index];
    }
    for (u32 slot = 0; slot < RESOURCE_SG_MAX; slot++) {
        struct sg_resource *resource = &sg_resources[slot];
        if (resource->active) continue;
        for (u32 index = 0; index < RESOURCE_SG_ENTRY_MAX; index++) {
            resource->entries[index].page = 0;
            resource->entries[index].physical = 0;
            resource->entries[index].page_index = 0;
            resource->entries[index].offset = 0;
            resource->entries[index].length = 0;
        }
        u32 pinned = 0;
        while (pinned < entry_count) {
            if (page_resource_pin(pages[pinned])) break;
            struct page_resource *page = page_resource_get(pages[pinned]);
            resource->entries[pinned].page = pages[pinned];
            resource->entries[pinned].physical =
                page->physical[page_indices[pinned]] + offsets[pinned];
            resource->entries[pinned].page_index = page_indices[pinned];
            resource->entries[pinned].offset = offsets[pinned];
            resource->entries[pinned].length = lengths[pinned];
            pinned++;
        }
        if (pinned != entry_count) {
            while (pinned) {
                pinned--;
                page_resource_unpin(resource->entries[pinned].page);
                resource->entries[pinned].page = 0;
            }
            return 0;
        }
        resource->entry_count = entry_count;
        resource->total_length = total;
        resource->revoked = 0;
        resource->active = 1;
        struct kernel_object *object = object_create(
            KOBJECT_SG_LIST, slot + 1, sg_destroy);
        if (!object) {
            resource->active = 0;
            while (pinned) {
                pinned--;
                page_resource_unpin(resource->entries[pinned].page);
                resource->entries[pinned].page = 0;
            }
            resource->entry_count = 0;
            resource->total_length = 0;
        }
        return object;
    }
    return 0;
}

struct sg_resource *sg_resource_get(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_SG_LIST ||
        !object->value || object->value > RESOURCE_SG_MAX)
        return 0;
    struct sg_resource *resource = &sg_resources[object->value - 1];
    return resource->active ? resource : 0;
}

int sg_resource_revoke(struct kernel_object *object) {
    struct sg_resource *resource = sg_resource_get(object);
    if (!resource || resource->revoked) return -1;
    resource->revoked = 1;
    int result = 0;
    for (u32 index = 0; index < resource->entry_count; index++) {
        if (!resource->entries[index].page) continue;
        if (page_resource_unpin(resource->entries[index].page)) result = -1;
        resource->entries[index].page = 0;
    }
    return result;
}

int sg_resource_usable(const struct kernel_object *object) {
    struct sg_resource *resource = sg_resource_get(object);
    if (!resource || resource->revoked) return 0;
    for (u32 index = 0; index < resource->entry_count; index++) {
        struct page_resource *page =
            page_resource_get(resource->entries[index].page);
        if (!page || page->revoked) return 0;
    }
    return 1;
}
