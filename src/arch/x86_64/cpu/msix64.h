#ifndef MSIX64_H
#define MSIX64_H

#include "types.h"

struct kernel_object;

int msix64_init(u8 destination_apic_id);
struct kernel_object *msix64_create(struct kernel_object *table, u32 entry);
int msix64_create_group(struct kernel_object *table, u32 first_entry,
                        u32 count, struct kernel_object **irqs,
                        u32 capacity);
int msix64_irq_entry(struct kernel_object *irq, struct kernel_object *pci,
                     u32 *entry_index);

#endif
