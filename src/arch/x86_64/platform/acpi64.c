#include "acpi64.h"

#define ACPI_PHYS_LIMIT 0x40000000ULL
#define ACPI_TABLE_MAX 0x100000U
#define ACPI_ROOT_MAX 256U

struct rsdp1 {
    char signature[8];
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 rsdt_address;
} __attribute__((packed));

struct rsdp2 {
    struct rsdp1 first;
    u32 length;
    u64 xsdt_address;
    u8 extended_checksum;
    u8 reserved[3];
} __attribute__((packed));

struct madt_header {
    struct acpi_sdt_header header;
    u32 local_apic_address;
    u32 flags;
} __attribute__((packed));

struct mcfg_record {
    u64 base;
    u16 segment;
    u8 start_bus;
    u8 end_bus;
    u32 reserved;
} __attribute__((packed));

static const struct acpi_sdt_header *root_table;
static int root_is_xsdt;
static const struct rsdp2 *firmware_rsdp;
static struct acpi_madt_info madt_info;
static struct acpi_mcfg_entry mcfg_entries[ACPI_MAX_MCFG];
static u32 mcfg_count;
static struct acpi_dmar_info dmar_info;
static int dmar_present;
static struct acpi_ivrs_info ivrs_info;
static int ivrs_present;

static int bytes_equal(const char *left, const char *right, u32 length) {
    for (u32 index = 0; index < length; index++)
        if (left[index] != right[index]) return 0;
    return 1;
}

static int range_valid(u64 address, u64 length) {
    return length && address < ACPI_PHYS_LIMIT &&
           length <= ACPI_PHYS_LIMIT - address;
}

static int checksum_valid(const void *pointer, u32 length) {
    const u8 *bytes = (const u8 *)pointer;
    u8 sum = 0;
    for (u32 index = 0; index < length; index++) sum += bytes[index];
    return sum == 0;
}

static const struct rsdp2 *scan_rsdp(u32 start, u32 end) {
    start = (start + 15) & ~15u;
    for (u32 address = start; address + sizeof(struct rsdp1) <= end;
         address += 16) {
        const struct rsdp2 *rsdp = (const struct rsdp2 *)(uptr_t)address;
        if (!bytes_equal(rsdp->first.signature, "RSD PTR ", 8) ||
            !checksum_valid(rsdp, sizeof(struct rsdp1)))
            continue;
        if (rsdp->first.revision < 2) return rsdp;
        if (rsdp->length < sizeof(struct rsdp2) || rsdp->length > 4096 ||
            !range_valid(address, rsdp->length) ||
            !checksum_valid(rsdp, rsdp->length))
            continue;
        return rsdp;
    }
    return 0;
}

void acpi64_set_rsdp(u32 pointer) {
    firmware_rsdp = pointer ? (const struct rsdp2 *)(uptr_t)pointer : 0;
}

static const struct rsdp2 *validate_rsdp(const struct rsdp2 *rsdp) {
    if (!rsdp || !bytes_equal(rsdp->first.signature, "RSD PTR ", 8) ||
        !checksum_valid(rsdp, sizeof(struct rsdp1)))
        return 0;
    if (rsdp->first.revision < 2) return rsdp;
    if (rsdp->length < sizeof(struct rsdp2) || rsdp->length > 4096 ||
        !range_valid((u64)(uptr_t)rsdp, rsdp->length) ||
        !checksum_valid(rsdp, rsdp->length))
        return 0;
    return rsdp;
}

static const struct rsdp2 *find_rsdp(void) {
    u16 ebda_segment;
    if (firmware_rsdp) return validate_rsdp(firmware_rsdp);
    __asm__ volatile("movw 0x40e, %0" : "=r"(ebda_segment));
    u32 ebda = (u32)ebda_segment << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000) {
        const struct rsdp2 *rsdp = scan_rsdp(ebda, ebda + 1024);
        if (rsdp) return rsdp;
    }
    return scan_rsdp(0xE0000, 0x100000);
}

static const struct acpi_sdt_header *table_at(u64 address) {
    if (!range_valid(address, sizeof(struct acpi_sdt_header))) return 0;
    const struct acpi_sdt_header *table =
        (const struct acpi_sdt_header *)(uptr_t)address;
    if (table->length < sizeof(*table) || table->length > ACPI_TABLE_MAX ||
        !range_valid(address, table->length) ||
        !checksum_valid(table, table->length))
        return 0;
    return table;
}

static u64 read_root_entry(const u8 *entry, int xsdt) {
    u64 value = 0;
    u32 length = xsdt ? 8 : 4;
    for (u32 index = 0; index < length; index++)
        value |= (u64)entry[index] << (index * 8);
    return value;
}

const struct acpi_sdt_header *acpi64_find(const char signature[4]) {
    if (!root_table) return 0;
    u32 entry_size = root_is_xsdt ? 8 : 4;
    u32 payload = root_table->length - sizeof(*root_table);
    if (payload % entry_size) return 0;
    u32 count = payload / entry_size;
    if (count > ACPI_ROOT_MAX) return 0;
    const u8 *entries = (const u8 *)root_table + sizeof(*root_table);
    for (u32 index = 0; index < count; index++) {
        u64 address = read_root_entry(entries + index * entry_size,
                                      root_is_xsdt);
        const struct acpi_sdt_header *table = table_at(address);
        if (table && bytes_equal(table->signature, signature, 4)) return table;
    }
    return 0;
}

static u16 read_le16(const u8 *bytes) {
    return (u16)bytes[0] | ((u16)bytes[1] << 8);
}

static u32 read_le32(const u8 *bytes) {
    return (u32)bytes[0] | ((u32)bytes[1] << 8) |
           ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24);
}

static u64 read_le64(const u8 *bytes) {
    u64 value = 0;
    for (u32 index = 0; index < 8; index++)
        value |= (u64)bytes[index] << (index * 8);
    return value;
}

static int parse_madt(void) {
    const struct madt_header *madt =
        (const struct madt_header *)acpi64_find("APIC");
    if (!madt || madt->header.length < sizeof(*madt)) return -1;
    madt_info.local_apic_address = madt->local_apic_address;
    madt_info.flags = madt->flags;
    madt_info.enabled_cpus = 0;
    madt_info.cpu_count = 0;
    madt_info.ioapic_count = 0;
    madt_info.iso_count = 0;
    const u8 *cursor = (const u8 *)madt + sizeof(*madt);
    const u8 *end = (const u8 *)madt + madt->header.length;
    while (cursor < end) {
        if ((usize_t)(end - cursor) < 2 || cursor[1] < 2 ||
            cursor[1] > (usize_t)(end - cursor))
            return -1;
        u8 type = cursor[0];
        u8 length = cursor[1];
        if (type == 0 && length >= 8) {
            u32 flags = (u32)cursor[4] | ((u32)cursor[5] << 8) |
                        ((u32)cursor[6] << 16) | ((u32)cursor[7] << 24);
            if (!(flags & 3)) continue;
            madt_info.enabled_cpus++;
            if (madt_info.cpu_count < ACPI_MAX_CPUS) {
                struct acpi_cpu_info *cpu =
                    &madt_info.cpus[madt_info.cpu_count++];
                cpu->acpi_id = cursor[0];
                cpu->apic_id = cursor[2];
                cpu->flags = flags;
            }
        } else if (type == 1 && length >= 12 &&
                   madt_info.ioapic_count < ACPI_MAX_IOAPICS) {
            u32 address = (u32)cursor[4] | ((u32)cursor[5] << 8) |
                          ((u32)cursor[6] << 16) | ((u32)cursor[7] << 24);
            if (!address || (address & 0xFFF)) return -1;
            struct acpi_ioapic_info *info =
                &madt_info.ioapics[madt_info.ioapic_count++];
            info->id = cursor[2];
            info->address = address;
            info->gsi_base = (u32)cursor[8] | ((u32)cursor[9] << 8) |
                             ((u32)cursor[10] << 16) | ((u32)cursor[11] << 24);
        } else if (type == 2 && length >= 10 &&
                   madt_info.iso_count < ACPI_MAX_ISO) {
            struct acpi_iso_info *info =
                &madt_info.overrides[madt_info.iso_count++];
            info->bus = cursor[2];
            info->source = cursor[3];
            info->gsi = (u32)cursor[4] | ((u32)cursor[5] << 8) |
                        ((u32)cursor[6] << 16) | ((u32)cursor[7] << 24);
            info->flags = (u16)cursor[8] | ((u16)cursor[9] << 8);
            u32 polarity = info->flags & 3;
            u32 trigger = (info->flags >> 2) & 3;
            if (polarity == 2 || trigger == 2) return -1;
        } else if (type == 5 && length >= 12) {
            u64 address = read_le64(cursor + 4);
            if (!address || (address & 0xFFF)) return -1;
            madt_info.local_apic_address = address;
        } else if (type == 9 && length >= 16) {
            u32 flags = (u32)cursor[8] | ((u32)cursor[9] << 8) |
                        ((u32)cursor[10] << 16) | ((u32)cursor[11] << 24);
            if (flags & 3) madt_info.enabled_cpus++;
        }
        cursor += length;
    }
    return madt_info.enabled_cpus && madt_info.local_apic_address ? 0 : -1;
}

static int parse_mcfg(void) {
    const struct acpi_sdt_header *mcfg = acpi64_find("MCFG");
    mcfg_count = 0;
    if (!mcfg) return 0;
    if (mcfg->length < sizeof(*mcfg) + 8) return -1;
    u32 payload = mcfg->length - sizeof(*mcfg) - 8;
    if (payload % sizeof(struct mcfg_record)) return -1;
    u32 count = payload / sizeof(struct mcfg_record);
    if (count > ACPI_MAX_MCFG) return -1;
    const struct mcfg_record *records = (const struct mcfg_record *)
        ((const u8 *)mcfg + sizeof(*mcfg) + 8);
    for (u32 index = 0; index < count; index++) {
        u64 buses = (u64)records[index].end_bus - records[index].start_bus + 1;
        u64 span = buses << 20;
        if (!records[index].base ||
            records[index].start_bus > records[index].end_bus ||
            (records[index].base & 0xFFFFFULL) ||
            span > ~0ULL - records[index].base)
            return -1;
        for (u32 previous = 0; previous < index; previous++) {
            if (mcfg_entries[previous].segment != records[index].segment)
                continue;
            if (records[index].start_bus <= mcfg_entries[previous].end_bus &&
                mcfg_entries[previous].start_bus <= records[index].end_bus)
                return -1;
        }
        mcfg_entries[index].base = records[index].base;
        mcfg_entries[index].segment = records[index].segment;
        mcfg_entries[index].start_bus = records[index].start_bus;
        mcfg_entries[index].end_bus = records[index].end_bus;
    }
    mcfg_count = count;
    return 0;
}

static int parse_dmar_scopes(const u8 *p, const u8 *end,
                             u32 *first, u32 *count) {
    *first = dmar_info.scope_count;
    *count = 0;
    while (p < end) {
        if ((usize_t)(end - p) < 6 || p[1] < 6 || p[1] > (usize_t)(end - p) ||
            ((p[1] - 6) & 1))
            return -1;
        u32 paths = (p[1] - 6) / 2;
        if (!p[0] || p[0] > 5 || paths > ACPI_DMAR_PATH_MAX ||
            dmar_info.scope_count >= ACPI_MAX_DMAR_SCOPES)
            return -1;
        struct acpi_dmar_scope *scope =
            &dmar_info.scopes[dmar_info.scope_count++];
        scope->type = p[0];
        scope->start_bus = p[5];
        scope->path_length = (u8)paths;
        for (u32 i = 0; i < paths; i++) {
            scope->device[i] = p[6 + i * 2];
            scope->function[i] = p[7 + i * 2];
            if (scope->device[i] > 31 || scope->function[i] > 7) return -1;
        }
        (*count)++;
        p += p[1];
    }
    return p == end ? 0 : -1;
}

static int parse_dmar(void) {
    const struct acpi_sdt_header *dmar = acpi64_find("DMAR");
    dmar_present = 0;
    dmar_info.unit_count = 0;
    dmar_info.rmrr_count = 0;
    dmar_info.scope_count = 0;
    if (!dmar) return 0;
    if (dmar->length < sizeof(*dmar) + 12) return -1;
    const u8 *base = (const u8 *)dmar;
    dmar_info.host_address_width = base[sizeof(*dmar)];
    dmar_info.flags = base[sizeof(*dmar) + 1];
    if (dmar_info.host_address_width < 31 ||
        dmar_info.host_address_width > 63)
        return -1;
    const u8 *p = base + sizeof(*dmar) + 12;
    const u8 *end = base + dmar->length;
    while (p < end) {
        if ((usize_t)(end - p) < 4) return -1;
        u16 type = read_le16(p);
        u16 len = read_le16(p + 2);
        if (len < 4 || len > (usize_t)(end - p)) return -1;
        if (type == 0) {
            if (len < 16 || dmar_info.unit_count >= ACPI_MAX_DMAR_UNITS)
                return -1;
            struct acpi_dmar_unit *unit =
                &dmar_info.units[dmar_info.unit_count++];
            unit->flags = p[4];
            unit->segment = read_le16(p + 6);
            unit->register_base = read_le64(p + 8);
            if (!unit->register_base || (unit->register_base & 0xFFF) ||
                parse_dmar_scopes(p + 16, p + len, &unit->scope_first,
                                  &unit->scope_count))
                return -1;
        } else if (type == 1) {
            if (len < 24 || dmar_info.rmrr_count >= ACPI_MAX_DMAR_RMRR)
                return -1;
            struct acpi_dmar_rmrr *rmrr =
                &dmar_info.rmrr[dmar_info.rmrr_count++];
            rmrr->segment = read_le16(p + 6);
            rmrr->base = read_le64(p + 8);
            rmrr->limit = read_le64(p + 16);
            if ((rmrr->base & 0xFFF) || (rmrr->limit & 0xFFF) ||
                rmrr->base > rmrr->limit ||
                parse_dmar_scopes(p + 24, p + len, &rmrr->scope_first,
                                  &rmrr->scope_count))
                return -1;
        } else if ((type == 2 && len < 8) ||
                   (type == 3 && len < 20) ||
                   (type == 4 && len < 8) || type > 4) {
            return -1;
        }
        p += len;
    }
    if (!dmar_info.unit_count) return -1;
    dmar_present = 1;
    return 0;
}

static int parse_ivrs(void) {
    const struct acpi_sdt_header *ivrs = acpi64_find("IVRS");
    ivrs_present = 0;
    ivrs_info.unit_count = 0;
    ivrs_info.range_count = 0;
    if (!ivrs) return 0;
    if (ivrs->length < sizeof(*ivrs) + 12) return -1;
    const u8 *base = (const u8 *)ivrs;
    ivrs_info.iv_info = read_le32(base + sizeof(*ivrs));
    const u8 *p = base + sizeof(*ivrs) + 12;
    const u8 *end = base + ivrs->length;
    while (p < end) {
        if ((usize_t)(end - p) < 4) return -1;
        u8 type = p[0];
        u16 len = read_le16(p + 2);
        if (len < 4 || len > (usize_t)(end - p)) return -1;
        if (type == 0x10 || type == 0x11 || type == 0x40) {
            u32 header = type == 0x10 ? 24 : 40;
            if (len < header || ivrs_info.unit_count >= ACPI_MAX_IVHD_UNITS)
                return -1;
            struct acpi_ivhd_unit *unit =
                &ivrs_info.units[ivrs_info.unit_count++];
            unit->type = type;
            unit->flags = p[1];
            unit->device_id = read_le16(p + 4);
            unit->capability_offset = read_le16(p + 6);
            unit->register_base = read_le64(p + 8);
            unit->segment = read_le16(p + 16);
            unit->iommu_info = read_le16(p + 18);
            unit->feature_info = read_le32(p + 20);
            if (!unit->register_base || (unit->register_base & 0xFFF) ||
                (unit->capability_offset & 3) ||
                unit->capability_offset > 0xFC)
                return -1;
            const u8 *entry = p + header;
            while (entry < p + len) {
                u32 size = entry[0] & 0x40 ? 8 : 4;
                if ((usize_t)(p + len - entry) < size) return -1;
                entry += size;
            }
        } else if (type >= 0x20 && type <= 0x22) {
            if (len < 32 || ivrs_info.range_count >= ACPI_MAX_IVMD_RANGES)
                return -1;
            struct acpi_ivmd_range *range =
                &ivrs_info.ranges[ivrs_info.range_count++];
            range->type = type;
            range->flags = p[1];
            range->device_id = read_le16(p + 4);
            range->auxiliary = read_le16(p + 6);
            range->base = read_le64(p + 16);
            u64 length = read_le64(p + 24);
            if (!length || (range->base & 0xFFF) ||
                length > ~0ULL - range->base)
                return -1;
            range->limit = range->base + length - 1;
        } else {
            return -1;
        }
        p += len;
    }
    if (!ivrs_info.unit_count) return -1;
    ivrs_present = 1;
    return 0;
}

int acpi64_init(void) {
    root_table = 0;
    root_is_xsdt = 0;
    mcfg_count = 0;
    dmar_present = 0;
    ivrs_present = 0;
    const struct rsdp2 *rsdp = find_rsdp();
    if (!rsdp) return -1;
    if (rsdp->first.revision >= 2 && rsdp->xsdt_address) {
        root_table = table_at(rsdp->xsdt_address);
        root_is_xsdt = 1;
        if (!root_table || !bytes_equal(root_table->signature, "XSDT", 4))
            return -1;
    } else {
        root_table = table_at(rsdp->first.rsdt_address);
        if (!root_table || !bytes_equal(root_table->signature, "RSDT", 4))
            return -1;
    }
    if (parse_madt() || parse_mcfg() || parse_dmar() || parse_ivrs()) return -1;
    return 0;
}

const struct acpi_madt_info *acpi64_madt(void) {
    return root_table ? &madt_info : 0;
}

u32 acpi64_mcfg_count(void) {
    return mcfg_count;
}

const struct acpi_mcfg_entry *acpi64_mcfg(u32 index) {
    return index < mcfg_count ? &mcfg_entries[index] : 0;
}

const struct acpi_dmar_info *acpi64_dmar(void) {
    return dmar_present ? &dmar_info : 0;
}

const struct acpi_ivrs_info *acpi64_ivrs(void) {
    return ivrs_present ? &ivrs_info : 0;
}
