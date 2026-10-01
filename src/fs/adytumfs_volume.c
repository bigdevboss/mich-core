#include "adytumfs_format.h"
#include "crc32c.h"
#include "block.h"
#include "cache.h"

int adytumfs_block_read(struct kernel_object *device, u64 block, u8 *buffer) {
    return block_cache_read(device, (u32)(block * ADYTUMFS_SECTORS_PER_BLOCK),
                            buffer, ADYTUMFS_SECTORS_PER_BLOCK);
}

int adytumfs_block_write(struct kernel_object *device, u64 block,
                         const u8 *buffer) {
    return block_cache_write(device, (u32)(block * ADYTUMFS_SECTORS_PER_BLOCK),
                             buffer, ADYTUMFS_SECTORS_PER_BLOCK);
}

// Zero source for newly allocated blocks; it is written to the device and
// never modified, so the bss zero it boots with is all it ever needs.
static u8 adytumfs_zero_block[ADYTUMFS_BLOCK_SIZE];

// One staging block for the checksum region's read-modify-write; the data
// paths are single-CPU, like the rest of the format layer.
static u8 adytumfs_checksum_scratch[ADYTUMFS_BLOCK_SIZE];

int adytumfs_data_check(struct kernel_object *device,
                        const struct adytumfs_superblock *super,
                        u64 block, const u8 *bytes) {
    if (block < super->data_start || block >= super->data_checksum_region)
        return -1;
    u64 index = block - super->data_start;
    u64 region_block =
        super->data_checksum_region + index / ADYTUMFS_CHECKSUMS_PER_BLOCK;
    if (adytumfs_block_read(device, region_block, adytumfs_checksum_scratch))
        return -1;
    const u8 *entry = adytumfs_checksum_scratch +
                      (index % ADYTUMFS_CHECKSUMS_PER_BLOCK) * 4u;
    return adytumfs_read_le32(entry) == crc32c(bytes, ADYTUMFS_BLOCK_SIZE)
               ? 0
               : -1;
}

int adytumfs_data_seal(struct kernel_object *device,
                       const struct adytumfs_superblock *super,
                       u64 block, const u8 *bytes) {
    if (block < super->data_start || block >= super->data_checksum_region)
        return -1;
    u64 index = block - super->data_start;
    u64 region_block =
        super->data_checksum_region + index / ADYTUMFS_CHECKSUMS_PER_BLOCK;
    if (adytumfs_block_read(device, region_block, adytumfs_checksum_scratch))
        return -1;
    adytumfs_write_le32(adytumfs_checksum_scratch +
                            (index % ADYTUMFS_CHECKSUMS_PER_BLOCK) * 4u,
                        crc32c(bytes, ADYTUMFS_BLOCK_SIZE));
    return adytumfs_block_write(device, region_block,
                                adytumfs_checksum_scratch);
}

int adytumfs_data_read(struct kernel_object *device,
                       const struct adytumfs_superblock *super,
                       u64 block, u8 *buffer) {
    if (adytumfs_block_read(device, block, buffer)) return -1;
    return adytumfs_data_check(device, super, block, buffer);
}

int adytumfs_data_write(struct kernel_object *device,
                        const struct adytumfs_superblock *super,
                        u64 block, const u8 *buffer) {
    if (adytumfs_block_write(device, block, buffer)) return -1;
    return adytumfs_data_seal(device, super, block, buffer);
}

int adytumfs_block_zero(struct kernel_object *device,
                        const struct adytumfs_superblock *super, u64 block) {
    u32 lba = (u32)(block * ADYTUMFS_SECTORS_PER_BLOCK);
    block_cache_invalidate(device, lba, ADYTUMFS_SECTORS_PER_BLOCK);
    for (u32 sector = 0; sector < ADYTUMFS_SECTORS_PER_BLOCK; sector++)
        if (block_io(device, BLOCK_OP_WRITE, lba + sector, 1,
                     adytumfs_zero_block + sector * BLOCK_SECTOR_SIZE,
                     BLOCK_SECTOR_SIZE))
            return -1;
    // A freshly allocated block reads back as zeroes through the verified
    // path, so the zero image has to be sealed like any other write.
    return adytumfs_data_seal(device, super, block, adytumfs_zero_block);
}

int adytumfs_make(struct kernel_object *device) {
    struct block_info info;
    if (!device || block_info(device, &info) ||
        (info.flags & BLOCK_FLAG_READ_ONLY))
        return -1;

    u64 total_blocks = info.sector_count / ADYTUMFS_SECTORS_PER_BLOCK;
    const u64 bits_per_block = (u64)ADYTUMFS_BLOCK_SIZE * 8;
    u64 bitmap_blocks = (total_blocks + bits_per_block - 1) / bits_per_block;
    // The table is bounded: the VFS mounts at most sixteen files, so a table
    // for a few hundred inodes is generous headroom, and the cap keeps the
    // format cost proportional on large volumes instead of zeroing a full
    // thirty-second of the device through the block cache.
    u64 inode_table_blocks = total_blocks / 32;
    if (inode_table_blocks > ADYTUMFS_INODE_TABLE_BLOCKS_MAX)
        inode_table_blocks = ADYTUMFS_INODE_TABLE_BLOCKS_MAX;
    if (inode_table_blocks == 0) inode_table_blocks = 1;
    u64 inode_table_start = 1 + bitmap_blocks;
    u64 data_start = inode_table_start + inode_table_blocks;
    // The tail from the last block backwards (shadow superblock, shadow
    // bitmap, shadow table) must leave room past the prefix for at least one
    // data block beside the checksum region.
    u64 tail_base = total_blocks - 1 - bitmap_blocks - inode_table_blocks;
    if (total_blocks < 1 || tail_base <= data_start) return -1;

    u64 inode_count = inode_table_blocks * ADYTUMFS_INODES_PER_BLOCK;
    // The checksum region ends exactly where the data region ends; its size
    // holds one u32 per data block, so region and data spans are solved
    // together against the space the tail leaves.
    u64 data_span = tail_base - data_start;
    u64 region_blocks = 1;
    while ((data_span - region_blocks + ADYTUMFS_CHECKSUMS_PER_BLOCK - 1) /
               ADYTUMFS_CHECKSUMS_PER_BLOCK >
           region_blocks)
        region_blocks++;
    u64 data_blocks = data_span - region_blocks;
    if (data_blocks == 0) return -1;
    u64 region_start = data_start + data_blocks;
    u64 shadow_table_start = tail_base - inode_table_blocks;
    u64 shadow_bitmap_start = tail_base;

    struct adytumfs_superblock super = {
        .format_version = ADYTUMFS_FORMAT_VERSION,
        .block_size = ADYTUMFS_BLOCK_SIZE,
        .total_blocks = total_blocks,
        .block_bitmap_start = 1,
        .block_bitmap_blocks = bitmap_blocks,
        .inode_table_start = inode_table_start,
        .inode_table_blocks = inode_table_blocks,
        .inode_count = inode_count,
        .root_inode = ADYTUMFS_ROOT_INODE,
        .data_start = data_start,
        .free_blocks = data_blocks,
        // Slot 0 is the null inode and the root occupies slot 1, so the
        // free count starts two below the table size.
        .free_inodes = inode_count - 2,
        .generation = 1,
        .feature_compat = 0,
        .feature_incompat = ADYTUMFS_FEATURE_INCOMPAT_V2,
        .feature_ro_compat = 0,
        .data_checksum_region = region_start,
    };

    // A 4 KiB staging block is too big for a modest stack, and the format path is
    // single threaded, so it is static.
    static u8 block[ADYTUMFS_BLOCK_SIZE];

    adytumfs_super_pack(block, &super);
    if (adytumfs_block_write(device, 0, block) ||
        adytumfs_block_write(device, total_blocks - 1, block))
        return -1;

    // The metadata prefix [0, data_start) and the whole tail from the checksum
    // region on are allocated; an empty root directory owns no data blocks yet.
    // Both bitmap copies carry the same map, so the shadow side starts life as
    // an exact twin of the primary.
    for (u64 bitmap = 0; bitmap < bitmap_blocks; bitmap++) {
        for (u32 index = 0; index < ADYTUMFS_BLOCK_SIZE; index++) block[index] = 0;
        u64 base = bitmap * bits_per_block;
        for (u64 used = 0; used < total_blocks; used++)
            if ((used < data_start || used >= region_start) &&
                used >= base && used - base < bits_per_block)
                block[(used - base) / 8] |= (u8)(1u << ((used - base) % 8));
        if (adytumfs_block_write(device, super.block_bitmap_start + bitmap, block) ||
            adytumfs_block_write(device, shadow_bitmap_start + bitmap, block))
            return -1;
    }

    // Both table copies hold the root, so a future commit that flips the
    // active superblock starts from a complete shadow set.
    for (u64 table = 0; table < inode_table_blocks; table++) {
        for (u32 index = 0; index < ADYTUMFS_BLOCK_SIZE; index++) block[index] = 0;
        if (table == 0) {
            struct adytumfs_inode root = {0};
            root.mode = ADYTUMFS_MODE_DIR | 0755u;
            root.links = 2;
            root.generation = 1;
            adytumfs_inode_pack(
                block + ADYTUMFS_ROOT_INODE * ADYTUMFS_INODE_SIZE, &root);
        }
        if (adytumfs_block_write(device, inode_table_start + table, block) ||
            adytumfs_block_write(device, shadow_table_start + table, block))
            return -1;
    }

    // A block nobody owns carries checksum zero: the first zeroing or write on
    // allocation replaces it, and an unallocated block is never read.
    for (u64 region = 0; region < region_blocks; region++)
        if (adytumfs_block_write(device, region_start + region,
                                 adytumfs_zero_block))
            return -1;

    return block_cache_flush(device);
}

// The bitmap block for a given data block is re-read on every bit access; the
// block cache absorbs the repeats, so it stays a read-modify-write against one
// staging block rather than holding the whole bitmap in memory.
static u8 adytumfs_bitmap_scratch[ADYTUMFS_BLOCK_SIZE];

static int adytumfs_bit_test(struct kernel_object *device,
                             const struct adytumfs_superblock *super,
                             u64 block) {
    const u64 bits_per_block = (u64)ADYTUMFS_BLOCK_SIZE * 8;
    u64 bitmap = block / bits_per_block;
    u64 offset = block % bits_per_block;
    if (bitmap >= super->block_bitmap_blocks) return -1;
    if (adytumfs_block_read(device, super->block_bitmap_start + bitmap,
                            adytumfs_bitmap_scratch))
        return -1;
    return (adytumfs_bitmap_scratch[offset / 8] >> (offset % 8)) & 1;
}

static int adytumfs_bit_write(struct kernel_object *device,
                              const struct adytumfs_superblock *super,
                              u64 block, int used) {
    const u64 bits_per_block = (u64)ADYTUMFS_BLOCK_SIZE * 8;
    u64 bitmap = block / bits_per_block;
    u64 offset = block % bits_per_block;
    if (bitmap >= super->block_bitmap_blocks) return -1;
    if (adytumfs_block_read(device, super->block_bitmap_start + bitmap,
                            adytumfs_bitmap_scratch))
        return -1;
    if (used)
        adytumfs_bitmap_scratch[offset / 8] |= (u8)(1u << (offset % 8));
    else
        adytumfs_bitmap_scratch[offset / 8] &= (u8)~(1u << (offset % 8));
    return adytumfs_block_write(device, super->block_bitmap_start + bitmap,
                                adytumfs_bitmap_scratch);
}

int adytumfs_alloc_run(struct kernel_object *device,
                       struct adytumfs_superblock *super,
                       u64 length, u64 *start) {
    if (!super || !start || length == 0) return -1;
    // Everything from the checksum region on is metadata (the region, the
    // shadow table, the shadow bitmap, the backup superblock), so it is never
    // allocatable.
    u64 usable_end = super->data_checksum_region;
    u64 run_start = 0;
    u64 run = 0;
    for (u64 block = super->data_start; block < usable_end; block++) {
        int used = adytumfs_bit_test(device, super, block);
        if (used < 0) return -1;
        if (used) {
            run = 0;
            continue;
        }
        if (run == 0) run_start = block;
        run++;
        if (run == length) {
            for (u64 mark = run_start; mark < run_start + length; mark++)
                if (adytumfs_bit_write(device, super, mark, 1)) return -1;
            super->free_blocks -= length;
            *start = run_start;
            return adytumfs_super_sync(device, super);
        }
    }
    return -1;
}

int adytumfs_free_run(struct kernel_object *device,
                      struct adytumfs_superblock *super,
                      u64 start, u64 length) {
    if (!super || length == 0) return -1;
    // Only data blocks are freeable; the metadata prefix and the tail from the
    // checksum region on must never be handed back to the allocator.
    if (start < super->data_start || start >= super->data_checksum_region ||
        length > super->data_checksum_region - start)
        return -1;
    for (u64 block = start; block < start + length; block++)
        if (adytumfs_bit_write(device, super, block, 0)) return -1;
    super->free_blocks += length;
    return adytumfs_super_sync(device, super);
}
