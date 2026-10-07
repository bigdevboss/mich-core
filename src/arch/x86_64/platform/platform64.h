#ifndef PLATFORM64_H
#define PLATFORM64_H

#include "types.h"

struct acpi_madt_info;
struct kernel_object;

int platform64_interrupts_init(const struct acpi_madt_info *madt);
void platform64_eoi(void);
// Open a legacy line the kernel itself owns. The console receive interrupt
// has no userspace driver, so no binding can unmask it through the resource
// layer; the line was routed at boot and only the controller mask has to go.
int platform64_irq_enable(u32 irq);
struct kernel_object *platform64_irq_object(u32 irq);

#endif
