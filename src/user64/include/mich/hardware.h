#ifndef MICH64_USER_HARDWARE_H
#define MICH64_USER_HARDWARE_H

#include "irq_group.h"

#define MICH_IRQ_GROUP_MAX IRQ_GROUP_MAX
#define mich_irq_group_request irq_group_request

int mich_pci_count(void);
int mich_pci_open(unsigned int index);
int mich_pci_bar_open(unsigned int pci_handle, unsigned int bar);
long mich_pci_config_read8(unsigned int pci_handle, unsigned int offset);
long mich_pci_config_read16(unsigned int pci_handle, unsigned int offset);
long mich_pci_config_read32(unsigned int pci_handle, unsigned int offset);
int mich_pci_set_command(unsigned int pci_handle, unsigned int offset,
                         unsigned int value);
int mich_dma_allocate(unsigned int pages, unsigned long long address_mask);
int mich_msi_open(unsigned int pci_handle);
int mich_msix_table_open(unsigned int pci_handle);
int mich_irq_open(unsigned int legacy_irq);
int mich_msix_irq_open(unsigned int table_handle, unsigned int entry);
int mich_msi_group_open(struct mich_irq_group_request *request);
int mich_msix_group_open(struct mich_irq_group_request *request);
int mich_mmio_map(unsigned int handle, unsigned long long virtual_address);
int mich_dma_map(unsigned int handle, unsigned long long virtual_address);
int mich_resource_unmap(unsigned long long virtual_address,
                        unsigned long long length);
long long mich_resource_length(unsigned int handle);

#endif
