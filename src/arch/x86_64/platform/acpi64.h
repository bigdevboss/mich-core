#ifndef ACPI64_H
#define ACPI64_H

#include "types.h"

#define ACPI_MAX_IOAPICS 8
#define ACPI_MAX_ISO 16
#define ACPI_MAX_CPUS 8
#define ACPI_MAX_MCFG 32
#define ACPI_MAX_DMAR_UNITS 8
#define ACPI_MAX_DMAR_RMRR 16
#define ACPI_MAX_DMAR_SCOPES 32
#define ACPI_DMAR_PATH_MAX 8
#define ACPI_MAX_IVHD_UNITS 8
#define ACPI_MAX_IVMD_RANGES 16

struct acpi_sdt_header {
    char signature[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
} __attribute__((packed));

struct acpi_ioapic_info {
    u8 id;
    u32 address;
    u32 gsi_base;
};

struct acpi_iso_info {
    u8 bus;
    u8 source;
    u32 gsi;
    u16 flags;
};

struct acpi_cpu_info {
    u32 acpi_id;
    u32 apic_id;
    u32 flags;
};

struct acpi_madt_info {
    u64 local_apic_address;
    u32 flags;
    u32 enabled_cpus;
    u32 cpu_count;
    struct acpi_cpu_info cpus[ACPI_MAX_CPUS];
    u32 ioapic_count;
    u32 iso_count;
    struct acpi_ioapic_info ioapics[ACPI_MAX_IOAPICS];
    struct acpi_iso_info overrides[ACPI_MAX_ISO];
};

struct acpi_mcfg_entry {
    u64 base;
    u16 segment;
    u8 start_bus;
    u8 end_bus;
};

struct acpi_dmar_scope {
    u8 type;
    u8 start_bus;
    u8 path_length;
    u8 device[ACPI_DMAR_PATH_MAX];
    u8 function[ACPI_DMAR_PATH_MAX];
};

struct acpi_dmar_unit {
    u64 register_base;
    u16 segment;
    u8 flags;
    u32 scope_first;
    u32 scope_count;
};

struct acpi_dmar_rmrr {
    u64 base;
    u64 limit;
    u16 segment;
    u32 scope_first;
    u32 scope_count;
};

struct acpi_dmar_info {
    u8 host_address_width;
    u8 flags;
    u32 unit_count;
    u32 rmrr_count;
    u32 scope_count;
    struct acpi_dmar_unit units[ACPI_MAX_DMAR_UNITS];
    struct acpi_dmar_rmrr rmrr[ACPI_MAX_DMAR_RMRR];
    struct acpi_dmar_scope scopes[ACPI_MAX_DMAR_SCOPES];
};

struct acpi_ivhd_unit {
    u64 register_base;
    u32 feature_info;
    u16 device_id;
    u16 segment;
    u16 iommu_info;
    u16 capability_offset;
    u8 type;
    u8 flags;
};

struct acpi_ivmd_range {
    u64 base;
    u64 limit;
    u16 device_id;
    u16 auxiliary;
    u8 type;
    u8 flags;
};

struct acpi_ivrs_info {
    u32 iv_info;
    u32 unit_count;
    u32 range_count;
    struct acpi_ivhd_unit units[ACPI_MAX_IVHD_UNITS];
    struct acpi_ivmd_range ranges[ACPI_MAX_IVMD_RANGES];
};

void acpi64_set_rsdp(u32 pointer);
int acpi64_init(void);
const struct acpi_sdt_header *acpi64_find(const char signature[4]);
const struct acpi_madt_info *acpi64_madt(void);
u32 acpi64_mcfg_count(void);
const struct acpi_mcfg_entry *acpi64_mcfg(u32 index);
const struct acpi_dmar_info *acpi64_dmar(void);
const struct acpi_ivrs_info *acpi64_ivrs(void);

#endif
