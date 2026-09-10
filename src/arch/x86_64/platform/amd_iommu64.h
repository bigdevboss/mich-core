#ifndef AMD_IOMMU64_H
#define AMD_IOMMU64_H

#include "types.h"

struct kernel_object;

int amd_iommu64_init(void);
int amd_iommu64_prepare(void);
int amd_iommu64_register_backend(void);
int amd_iommu64_present(void);
int amd_iommu64_tables_ready(void);
u32 amd_iommu64_unit_count(void);
int amd_iommu64_test_domain(struct kernel_object *pci);

#endif
