#ifndef PCI64_H
#define PCI64_H

#include "types.h"

struct kernel_object;

int pci64_init(void);
int pci64_uses_ecam(void);
u32 pci64_count(void);
struct kernel_object *pci64_object(u32 index);
struct kernel_object *pci64_bar_create(struct kernel_object *pci, u32 bar);
int pci64_config_read8(struct kernel_object *pci, u16 offset, u8 *value);
int pci64_config_read16(struct kernel_object *pci, u16 offset, u16 *value);
int pci64_config_read32(struct kernel_object *pci, u16 offset, u32 *value);
int pci64_config_write8(struct kernel_object *pci, u16 offset, u8 value);
int pci64_config_write16(struct kernel_object *pci, u16 offset, u16 value);
int pci64_config_write32(struct kernel_object *pci, u16 offset, u32 value);
int pci64_set_command(struct kernel_object *pci, u16 set_bits,
                      u16 clear_bits);
int pci64_quiesce(struct kernel_object *pci);
int pci64_reset(struct kernel_object *pci);
int pci64_msi_enable(struct kernel_object *pci, u8 vector, u8 destination);
int pci64_msi_enable_group(struct kernel_object *pci, u8 vector,
                           u32 count, u8 destination);
u32 pci64_msi_max_vectors(struct kernel_object *pci);
int pci64_msi_disable(struct kernel_object *pci);
int pci64_msix_configure(struct kernel_object *pci, int enabled,
                         int function_masked);
struct kernel_object *pci64_msix_table_create(struct kernel_object *pci);

#endif
