#ifndef MSI64_H
#define MSI64_H

#include "types.h"

struct kernel_object;

int msi64_init(u8 destination_apic_id);
struct kernel_object *msi64_create(struct kernel_object *pci);
int msi64_create_group(struct kernel_object *pci, u32 count,
                       struct kernel_object **irqs, u32 capacity);

#endif
