#ifndef PLATFORM64_H
#define PLATFORM64_H

#include "types.h"

struct acpi_madt_info;
struct kernel_object;

int platform64_interrupts_init(const struct acpi_madt_info *madt);
void platform64_eoi(void);
struct kernel_object *platform64_irq_object(u32 irq);

#endif
