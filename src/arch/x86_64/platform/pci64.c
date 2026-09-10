#include "pci64.h"
#include "resource.h"
#include "object.h"
#include "vector64.h"
#include "acpi64.h"
#include "vm64.h"
#include "apic64.h"

#define PCI64_MAX_OBJECTS 64

static struct kernel_object *pci_objects[PCI64_MAX_OBJECTS];
static u32 pci_count;
static int ecam_enabled;
static u16 active_segment;
static u16 mapped_segment;
static u8 mapped_bus;
static void *mapped_ecam;

static inline void outl(u16 port, u32 value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outb(u16 port, u8 value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(u16 port, u16 value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline u32 inl(u16 port) {
    u32 value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static u32 config_address(u8 bus, u8 device, u8 function, u8 offset) {
    return 0x80000000u | ((u32)bus << 16) | ((u32)device << 11) |
           ((u32)function << 8) | (offset & 0xFCu);
}

static volatile u8 *ecam_bus(u16 segment, u8 bus) {
    if (mapped_ecam && mapped_segment == segment && mapped_bus == bus)
        return (volatile u8 *)mapped_ecam;
    if (mapped_ecam) {
        vm64_iounmap(mapped_ecam, 0x100000);
        mapped_ecam = 0;
    }
    for (u32 index = 0; index < acpi64_mcfg_count(); index++) {
        const struct acpi_mcfg_entry *entry = acpi64_mcfg(index);
        if (!entry || entry->segment != segment || bus < entry->start_bus ||
            bus > entry->end_bus)
            continue;
        u64 physical = entry->base + ((u64)bus - entry->start_bus) * 0x100000;
        mapped_ecam = vm64_ioremap(physical, 0x100000, VM64_CACHE_UC);
        if (!mapped_ecam) return 0;
        mapped_segment = segment;
        mapped_bus = bus;
        return (volatile u8 *)mapped_ecam;
    }
    return 0;
}

static u32 config_read32(u8 bus, u8 device, u8 function, u8 offset) {
    if (ecam_enabled) {
        volatile u8 *base = ecam_bus(active_segment, bus);
        if (!base) return 0xFFFFFFFFu;
        uptr_t address = (uptr_t)base + ((uptr_t)device << 15) +
                         ((uptr_t)function << 12) + (offset & 0xFCu);
        return *(volatile u32 *)address;
    }
    outl(0xCF8, config_address(bus, device, function, offset));
    return inl(0xCFC);
}

static u16 config_read16(u8 bus, u8 device, u8 function, u8 offset) {
    u32 value = config_read32(bus, device, function, offset);
    return (u16)(value >> ((offset & 2) * 8));
}

static u8 config_read8(u8 bus, u8 device, u8 function, u8 offset) {
    u32 value = config_read32(bus, device, function, offset);
    return (u8)(value >> ((offset & 3) * 8));
}

static void config_write32(u8 bus, u8 device, u8 function,
                           u8 offset, u32 value) {
    if (ecam_enabled) {
        volatile u8 *base = ecam_bus(active_segment, bus);
        if (!base) return;
        uptr_t address = (uptr_t)base + ((uptr_t)device << 15) +
                         ((uptr_t)function << 12) + (offset & 0xFCu);
        *(volatile u32 *)address = value;
        return;
    }
    outl(0xCF8, config_address(bus, device, function, offset));
    outl(0xCFC, value);
}

static void config_write16(u8 bus, u8 device, u8 function,
                           u8 offset, u16 value) {
    if (offset & 1) return;
    if (ecam_enabled) {
        volatile u8 *base = ecam_bus(active_segment, bus);
        if (!base) return;
        uptr_t address = (uptr_t)base + ((uptr_t)device << 15) +
                         ((uptr_t)function << 12) + (offset & 0xFEu);
        *(volatile u16 *)address = value;
        return;
    }
    outl(0xCF8, config_address(bus, device, function, offset));
    outw((u16)(0xCFC + (offset & 2)), value);
}

static void config_write8(u8 bus, u8 device, u8 function,
                          u8 offset, u8 value) {
    if (ecam_enabled) {
        volatile u8 *base = ecam_bus(active_segment, bus);
        if (!base) return;
        uptr_t address = (uptr_t)base + ((uptr_t)device << 15) +
                         ((uptr_t)function << 12) + offset;
        *(volatile u8 *)address = value;
        return;
    }
    outl(0xCF8, config_address(bus, device, function, offset));
    outb((u16)(0xCFC + (offset & 3)), value);
}

static void clear_bars(struct pci_resource *resource) {
    for (u32 index = 0; index < 6; index++) {
        resource->bars[index].address = 0;
        resource->bars[index].length = 0;
        resource->bars[index].flags = 0;
    }
}

static void probe_bars(struct pci_resource *resource, u8 header_type) {
    u32 count = header_type == 0 ? 6 : header_type == 1 ? 2 : 0;
    u16 command = config_read16(resource->bus, resource->device,
                                resource->function, 0x04);
    config_write16(resource->bus, resource->device, resource->function,
                   0x04, command & (u16)~7u);
    for (u32 index = 0; index < count; index++) {
        u8 offset = (u8)(0x10 + index * 4);
        u32 original = config_read32(resource->bus, resource->device,
                                     resource->function, offset);
        if (original & 1) {
            config_write32(resource->bus, resource->device,
                           resource->function, offset, 0xFFFFFFFFu);
            u32 mask = config_read32(resource->bus, resource->device,
                                     resource->function, offset) & ~3u;
            config_write32(resource->bus, resource->device,
                           resource->function, offset, original);
            if (mask) {
                resource->bars[index].address = original & ~3u;
                resource->bars[index].length = (u32)(~mask + 1);
                resource->bars[index].flags = 1;
            }
            continue;
        }
        u32 kind = (original >> 1) & 3;
        u32 flags = (original & 8) ? 2 : 0;
        if (kind == 2 && index + 1 < count) {
            u32 original_high = config_read32(resource->bus, resource->device,
                                              resource->function, offset + 4);
            config_write32(resource->bus, resource->device,
                           resource->function, offset, 0xFFFFFFFFu);
            config_write32(resource->bus, resource->device,
                           resource->function, offset + 4, 0xFFFFFFFFu);
            u32 mask_low = config_read32(resource->bus, resource->device,
                                         resource->function, offset) & ~0xFu;
            u32 mask_high = config_read32(resource->bus, resource->device,
                                          resource->function, offset + 4);
            config_write32(resource->bus, resource->device,
                           resource->function, offset, original);
            config_write32(resource->bus, resource->device,
                           resource->function, offset + 4, original_high);
            u64 mask = ((u64)mask_high << 32) | mask_low;
            if (mask) {
                resource->bars[index].address =
                    ((u64)original_high << 32) | (original & ~0xFu);
                resource->bars[index].length = ~mask + 1;
                resource->bars[index].flags = flags | 4;
            }
            index++;
            continue;
        }
        config_write32(resource->bus, resource->device,
                       resource->function, offset, 0xFFFFFFFFu);
        u32 mask = config_read32(resource->bus, resource->device,
                                 resource->function, offset) & ~0xFu;
        config_write32(resource->bus, resource->device,
                       resource->function, offset, original);
        if (mask) {
            resource->bars[index].address = original & ~0xFu;
            resource->bars[index].length = (u32)(~mask + 1);
            resource->bars[index].flags = flags;
        }
    }
    config_write16(resource->bus, resource->device, resource->function,
                   0x04, command);
}

static int probe_capabilities(struct pci_resource *resource) {
    resource->capability_flags = 0;
    resource->msi_offset = 0;
    resource->msix_offset = 0;
    resource->pcie_offset = 0;
    u16 status = config_read16(resource->bus, resource->device,
                               resource->function, 0x06);
    if (!(status & 0x10)) return 0;
    u8 seen[32];
    for (u32 index = 0; index < sizeof(seen); index++) seen[index] = 0;
    u8 pointer = config_read8(resource->bus, resource->device,
                              resource->function, 0x34);
    for (u32 depth = 0; pointer && depth < 48; depth++) {
        if (pointer < 0x40 || pointer > 0xFC || (pointer & 3) ||
            (seen[pointer / 8] & (u8)(1u << (pointer % 8))))
            return -1;
        seen[pointer / 8] |= (u8)(1u << (pointer % 8));
        u8 id = config_read8(resource->bus, resource->device,
                             resource->function, pointer);
        if (id == 0x05) {
            resource->capability_flags |= PCI_CAP_MSI;
            resource->msi_offset = pointer;
        } else if (id == 0x10) {
            if (pointer > 0xF4) return -1;
            resource->capability_flags |= PCI_CAP_PCIE;
            resource->pcie_offset = pointer;
            u32 device_capabilities = config_read32(
                resource->bus, resource->device, resource->function,
                pointer + 4);
            if (device_capabilities & (1u << 28))
                resource->capability_flags |= PCI_CAP_FLR;
        } else if (id == 0x11) {
            resource->capability_flags |= PCI_CAP_MSIX;
            resource->msix_offset = pointer;
        }
        u8 next = config_read8(resource->bus, resource->device,
                               resource->function, pointer + 1);
        if (next & 3) return -1;
        pointer = next;
    }
    return pointer ? -1 : 0;
}

static int add_function(u16 segment, u8 bus, u8 device, u8 function) {
    active_segment = segment;
    u16 vendor = config_read16(bus, device, function, 0x00);
    if (vendor == 0xFFFF) return 0;
    if (pci_count >= PCI64_MAX_OBJECTS) return -1;
    struct pci_resource description;
    description.segment = segment;
    description.bus = bus;
    description.device = device;
    description.function = function;
    description.vendor_id = vendor;
    description.device_id = config_read16(bus, device, function, 0x02);
    description.revision = config_read8(bus, device, function, 0x08);
    description.programming_interface = config_read8(bus, device, function, 0x09);
    description.subclass = config_read8(bus, device, function, 0x0A);
    description.class_code = config_read8(bus, device, function, 0x0B);
    description.capability_flags = 0;
    description.msi_offset = 0;
    description.msix_offset = 0;
    description.pcie_offset = 0;
    description.active = 0;
    clear_bars(&description);
    u8 header_type = config_read8(bus, device, function, 0x0E) & 0x7F;
    probe_bars(&description, header_type);
    if (probe_capabilities(&description)) return -1;
    struct kernel_object *object = pci_resource_create(&description);
    if (!object) return -1;
    pci_objects[pci_count++] = object;
    return 1;
}

int pci64_init(void) {
    pci_count = 0;
    mapped_ecam = 0;
    mapped_segment = 0;
    mapped_bus = 0;
    ecam_enabled = acpi64_mcfg_count() != 0;
    if (ecam_enabled) {
        for (u32 range = 0; range < acpi64_mcfg_count(); range++) {
            const struct acpi_mcfg_entry *entry = acpi64_mcfg(range);
            if (!entry) return -1;
            for (u32 bus = entry->start_bus; bus <= entry->end_bus; bus++) {
                for (u32 device = 0; device < 32; device++) {
                    int first = add_function(entry->segment, (u8)bus,
                                             (u8)device, 0);
                    if (first < 0) return -1;
                    if (!first) continue;
                    active_segment = entry->segment;
                    u8 header = config_read8((u8)bus, (u8)device, 0, 0x0E);
                    if (!(header & 0x80)) continue;
                    for (u32 function = 1; function < 8; function++)
                        if (add_function(entry->segment, (u8)bus,
                                         (u8)device, (u8)function) < 0)
                            return -1;
                }
            }
        }
        if (mapped_ecam) {
            vm64_iounmap(mapped_ecam, 0x100000);
            mapped_ecam = 0;
        }
    } else {
        active_segment = 0;
        for (u32 bus = 0; bus < 256; bus++) {
            for (u32 device = 0; device < 32; device++) {
                int first = add_function(0, (u8)bus, (u8)device, 0);
                if (first < 0) return -1;
                if (!first) continue;
                u8 header = config_read8((u8)bus, (u8)device, 0, 0x0E);
                if (!(header & 0x80)) continue;
                for (u32 function = 1; function < 8; function++)
                    if (add_function(0, (u8)bus, (u8)device,
                                     (u8)function) < 0)
                        return -1;
            }
        }
    }
    return pci_count ? 0 : -1;
}

u32 pci64_count(void) {
    return pci_count;
}

struct kernel_object *pci64_object(u32 index) {
    return index < pci_count ? pci_objects[index] : 0;
}

struct kernel_object *pci64_bar_create(struct kernel_object *pci, u32 index) {
    const struct pci_resource *description = pci_resource_get(pci);
    if (!description || index >= 6) return 0;
    const struct pci_bar_resource *bar = &description->bars[index];
    if (!bar->address || !bar->length || (bar->flags & 1) ||
        (bar->address & 0xFFF) || (bar->length & 0xFFF))
        return 0;
    u32 cache = (bar->flags & 2) ? MMIO_CACHE_WC : MMIO_CACHE_UC;
    return mmio_resource_create(bar->address, bar->length, cache);
}

static const struct pci_resource *select_config(struct kernel_object *pci,
                                                u16 offset, u32 width) {
    const struct pci_resource *resource = pci_resource_get(pci);
    if (!resource || offset >= 256 || width > 256u - offset ||
        (width == 2 && (offset & 1)) || (width == 4 && (offset & 3)))
        return 0;
    active_segment = resource->segment;
    if (ecam_enabled && !ecam_bus(resource->segment, resource->bus)) return 0;
    return resource;
}

int pci64_config_read8(struct kernel_object *pci, u16 offset, u8 *value) {
    const struct pci_resource *resource = select_config(pci, offset, 1);
    if (!resource || !value) return -1;
    *value = config_read8(resource->bus, resource->device,
                          resource->function, (u8)offset);
    return 0;
}

int pci64_config_read16(struct kernel_object *pci, u16 offset, u16 *value) {
    const struct pci_resource *resource = select_config(pci, offset, 2);
    if (!resource || !value) return -1;
    *value = config_read16(resource->bus, resource->device,
                           resource->function, (u8)offset);
    return 0;
}

int pci64_config_read32(struct kernel_object *pci, u16 offset, u32 *value) {
    const struct pci_resource *resource = select_config(pci, offset, 4);
    if (!resource || !value) return -1;
    *value = config_read32(resource->bus, resource->device,
                           resource->function, (u8)offset);
    return 0;
}

int pci64_config_write8(struct kernel_object *pci, u16 offset, u8 value) {
    const struct pci_resource *resource = select_config(pci, offset, 1);
    if (!resource) return -1;
    config_write8(resource->bus, resource->device,
                  resource->function, (u8)offset, value);
    return 0;
}

int pci64_config_write16(struct kernel_object *pci, u16 offset, u16 value) {
    const struct pci_resource *resource = select_config(pci, offset, 2);
    if (!resource) return -1;
    config_write16(resource->bus, resource->device,
                   resource->function, (u8)offset, value);
    return 0;
}

int pci64_config_write32(struct kernel_object *pci, u16 offset, u32 value) {
    const struct pci_resource *resource = select_config(pci, offset, 4);
    if (!resource) return -1;
    config_write32(resource->bus, resource->device,
                   resource->function, (u8)offset, value);
    return 0;
}

int pci64_set_command(struct kernel_object *pci, u16 set_bits,
                      u16 clear_bits) {
    const struct pci_resource *resource = pci_resource_get(pci);
    if (!resource || (set_bits & clear_bits) ||
        ((set_bits | clear_bits) & (u16)~7u))
        return -1;
    active_segment = resource->segment;
    u16 command = config_read16(resource->bus, resource->device,
                                resource->function, 0x04);
    command = (command | set_bits) & (u16)~clear_bits;
    config_write16(resource->bus, resource->device, resource->function,
                   0x04, command);
    u16 verified = config_read16(resource->bus, resource->device,
                                 resource->function, 0x04);
    return (verified & 7u) == (command & 7u) ? 0 : -1;
}

int pci64_quiesce(struct kernel_object *pci) {
    const struct pci_resource *resource = pci_resource_get(pci);
    if (!resource) return -1;
    if (resource->capability_flags & PCI_CAP_MSI) pci64_msi_disable(pci);
    if (resource->capability_flags & PCI_CAP_MSIX)
        pci64_msix_configure(pci, 0, 1);
    return pci64_set_command(pci, 0, 4);
}

int pci64_reset(struct kernel_object *pci) {
    const struct pci_resource *resource = pci_resource_get(pci);
    if (!resource) return -1;
    if (!(resource->capability_flags & PCI_CAP_FLR)) return 1;
    if (!resource->pcie_offset || resource->pcie_offset > 0xF4 ||
        pci64_quiesce(pci))
        return -1;
    active_segment = resource->segment;
    u8 control_offset = resource->pcie_offset + 8;
    u16 control = config_read16(resource->bus, resource->device,
                                resource->function, control_offset);
    config_write16(resource->bus, resource->device, resource->function,
                   control_offset, control | (1u << 15));
    if (apic64_delay_ms(100)) return -1;
    u16 vendor = config_read16(resource->bus, resource->device,
                               resource->function, 0x00);
    if (vendor != resource->vendor_id) return -1;
    u16 command = config_read16(resource->bus, resource->device,
                                resource->function, 0x04);
    return command & 4 ? -1 : 0;
}

u32 pci64_msi_max_vectors(struct kernel_object *pci) {
    const struct pci_resource *resource = pci_resource_get(pci);
    if (resource) active_segment = resource->segment;
    if (!resource || !(resource->capability_flags & PCI_CAP_MSI) ||
        !resource->msi_offset || resource->msi_offset > 0xE8)
        return 0;
    u16 control = config_read16(resource->bus, resource->device,
                                resource->function, resource->msi_offset + 2);
    u32 shift = (control >> 1) & 7u;
    return shift > 5 ? 32 : 1u << shift;
}

int pci64_msi_enable_group(struct kernel_object *pci, u8 vector,
                           u32 count, u8 destination) {
    const struct pci_resource *resource = pci_resource_get(pci);
    if (resource) active_segment = resource->segment;
    if (!resource || !(resource->capability_flags & PCI_CAP_MSI) ||
        !resource->msi_offset || resource->msi_offset > 0xE8 ||
        vector < VECTOR64_MSI_FIRST || vector > VECTOR64_MSI_LAST ||
        !count || count > 32 || (count & (count - 1)) ||
        vector % count || vector + count - 1 > VECTOR64_MSI_LAST)
        return -1;
    u8 offset = resource->msi_offset;
    u16 control = config_read16(resource->bus, resource->device,
                                resource->function, offset + 2);
    u32 supported = 1u << ((control >> 1) & 7u);
    if (supported > 32) supported = 32;
    if (count > supported) return -1;
    config_write16(resource->bus, resource->device, resource->function,
                   offset + 2, control & (u16)~1u);
    u32 address = 0xFEE00000u | ((u32)destination << 12);
    config_write32(resource->bus, resource->device, resource->function,
                   offset + 4, address);
    u8 data_offset;
    if (control & (1u << 7)) {
        config_write32(resource->bus, resource->device, resource->function,
                       offset + 8, 0);
        data_offset = offset + 12;
    } else {
        data_offset = offset + 8;
    }
    config_write16(resource->bus, resource->device, resource->function,
                   data_offset, vector);
    u32 multiple = 0;
    while ((1u << multiple) < count) multiple++;
    control &= (u16)~0x70u;
    control |= (u16)(multiple << 4) | 1u;
    config_write16(resource->bus, resource->device, resource->function,
                   offset + 2, control);
    return 0;
}

int pci64_msi_enable(struct kernel_object *pci, u8 vector, u8 destination) {
    return pci64_msi_enable_group(pci, vector, 1, destination);
}

int pci64_msi_disable(struct kernel_object *pci) {
    const struct pci_resource *resource = pci_resource_get(pci);
    if (resource) active_segment = resource->segment;
    if (!resource || !(resource->capability_flags & PCI_CAP_MSI) ||
        !resource->msi_offset || resource->msi_offset > 0xE8)
        return -1;
    u8 offset = resource->msi_offset;
    u16 control = config_read16(resource->bus, resource->device,
                                resource->function, offset + 2);
    config_write16(resource->bus, resource->device, resource->function,
                   offset + 2, control & (u16)~1u);
    return 0;
}

struct kernel_object *pci64_msix_table_create(struct kernel_object *pci) {
    const struct pci_resource *resource = pci_resource_get(pci);
    if (resource) active_segment = resource->segment;
    if (!resource || !(resource->capability_flags & PCI_CAP_MSIX) ||
        !resource->msix_offset || resource->msix_offset > 0xF4)
        return 0;
    u8 offset = resource->msix_offset;
    u16 control = config_read16(resource->bus, resource->device,
                                resource->function, offset + 2);
    u32 entries = (control & 0x7FFu) + 1;
    u32 table = config_read32(resource->bus, resource->device,
                              resource->function, offset + 4);
    u32 pba = config_read32(resource->bus, resource->device,
                            resource->function, offset + 8);
    u8 table_bir = table & 7;
    u8 pba_bir = pba & 7;
    u32 table_offset = table & ~7u;
    u32 pba_offset = pba & ~7u;
    if (table_bir >= 6 || pba_bir >= 6) return 0;
    const struct pci_bar_resource *table_bar = &resource->bars[table_bir];
    const struct pci_bar_resource *pba_bar = &resource->bars[pba_bir];
    u64 table_bytes = (u64)entries * 16;
    u64 pba_bytes = ((u64)entries + 63) / 64 * 8;
    if ((table_bar->flags & 1) || (pba_bar->flags & 1) ||
        !table_bar->length || !pba_bar->length ||
        table_offset > table_bar->length ||
        table_bytes > table_bar->length - table_offset ||
        pba_offset > pba_bar->length ||
        pba_bytes > pba_bar->length - pba_offset)
        return 0;
    return msix_table_resource_create(pci, table_bir, table_offset,
                                      pba_bir, pba_offset, entries);
}

int pci64_msix_configure(struct kernel_object *pci, int enabled,
                         int function_masked) {
    const struct pci_resource *resource = pci_resource_get(pci);
    if (resource) active_segment = resource->segment;
    if (!resource || !(resource->capability_flags & PCI_CAP_MSIX) ||
        !resource->msix_offset || resource->msix_offset > 0xF4)
        return -1;
    u8 offset = resource->msix_offset;
    u16 control = config_read16(resource->bus, resource->device,
                                resource->function, offset + 2);
    if (enabled) control |= 1u << 15;
    else control &= (u16)~(1u << 15);
    if (function_masked) control |= 1u << 14;
    else control &= (u16)~(1u << 14);
    config_write16(resource->bus, resource->device, resource->function,
                   offset + 2, control);
    return 0;
}


int pci64_uses_ecam(void) {
    return ecam_enabled;
}
