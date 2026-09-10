#ifndef IOMMU_H
#define IOMMU_H

#include "types.h"

struct kernel_object;

struct iommu_fault {
    u64 address;
    u32 owner;
    u16 segment;
    u16 source_id;
    u8 reason;
    u8 write;
};

struct iommu_backend {
    const char *name;
    int (*enable)(void);
    int (*enabled)(void);
    int (*domain_create)(u32 owner, struct kernel_object *pci);
    int (*domain_exists)(u32 owner);
    int (*domain_map)(u32 owner, paddr_t physical, u32 pages, u64 *iova);
    int (*domain_unmap)(u32 owner, u64 iova, u32 pages);
    int (*domain_suspend)(u32 owner);
    int (*domain_resume)(u32 owner);
    int (*domain_destroy)(u32 owner);
    int (*fault_poll)(struct iommu_fault *fault);
};

void iommu_init(void);
int iommu_register(const struct iommu_backend *backend);
int iommu_present(void);
const char *iommu_name(void);
int iommu_enable(void);
int iommu_enabled(void);
int iommu_domain_create(u32 owner, struct kernel_object *pci);
int iommu_domain_exists(u32 owner);
int iommu_domain_map(u32 owner, paddr_t physical, u32 pages, u64 *iova);
int iommu_domain_unmap(u32 owner, u64 iova, u32 pages);
int iommu_domain_suspend(u32 owner);
int iommu_domain_resume(u32 owner);
int iommu_domain_destroy(u32 owner);
int iommu_dma_release(u32 owner, u64 iova, u32 pages);
int iommu_fault_poll(struct iommu_fault *fault);

#endif
