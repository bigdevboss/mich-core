#ifndef IOAPIC64_H
#define IOAPIC64_H

#include "types.h"

struct acpi_madt_info;

int ioapic64_init(const struct acpi_madt_info *madt);
int ioapic64_route(u32 gsi, u8 vector, u8 destination,
                   int level, int active_low, int masked);
int ioapic64_mask(u32 gsi, int masked);

#endif
