#include "iommu.h"

static const struct iommu_backend *active_backend;

void iommu_init(void) {
    active_backend = 0;
}

int iommu_register(const struct iommu_backend *backend) {
    if (!backend || !backend->name || !backend->name[0] || active_backend ||
        !backend->enable || !backend->enabled || !backend->domain_create ||
        !backend->domain_exists || !backend->domain_map ||
        !backend->domain_unmap || !backend->domain_suspend ||
        !backend->domain_resume || !backend->domain_destroy ||
        !backend->fault_poll)
        return -1;
    active_backend = backend;
    return 0;
}

int iommu_present(void) {
    return active_backend != 0;
}

const char *iommu_name(void) {
    return active_backend ? active_backend->name : 0;
}

int iommu_enable(void) {
    return active_backend ? active_backend->enable() : -1;
}

int iommu_enabled(void) {
    return active_backend && active_backend->enabled();
}

int iommu_domain_create(u32 owner, struct kernel_object *pci) {
    return active_backend ? active_backend->domain_create(owner, pci) : -1;
}

int iommu_domain_exists(u32 owner) {
    return active_backend && active_backend->domain_exists(owner);
}

int iommu_domain_map(u32 owner, paddr_t physical, u32 pages, u64 *iova) {
    return active_backend
        ? active_backend->domain_map(owner, physical, pages, iova) : -1;
}

int iommu_domain_unmap(u32 owner, u64 iova, u32 pages) {
    return active_backend
        ? active_backend->domain_unmap(owner, iova, pages) : -1;
}

int iommu_domain_suspend(u32 owner) {
    return active_backend ? active_backend->domain_suspend(owner) : -1;
}

int iommu_domain_resume(u32 owner) {
    return active_backend ? active_backend->domain_resume(owner) : -1;
}

int iommu_domain_destroy(u32 owner) {
    return active_backend ? active_backend->domain_destroy(owner) : -1;
}

int iommu_dma_release(u32 owner, u64 iova, u32 pages) {
    return iommu_domain_unmap(owner, iova, pages);
}

int iommu_fault_poll(struct iommu_fault *fault) {
    return active_backend ? active_backend->fault_poll(fault) : 0;
}
