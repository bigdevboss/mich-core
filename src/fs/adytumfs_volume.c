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

int adytumfs_make(struct kernel_object *device) {
    struct block_info info;
    if (!device || block_info(device, &info) ||
        (info.flags & BLOCK_FLAG_READ_ONLY))
        return -1;

    u64 total_blocks = info.sector_count / ADYTUMFS_SECTORS_PER_BLOCK;
    const u64 bits_per_block = (u64)ADYTUMFS_BLOCK_SIZE * 8;
    u64 bitmap_blocks = (total_blocks + bits_per_block - 1) / bits_per_block;
    u64 inode_table_blocks = total_blocks / 32;
    if (inode_table_blocks == 0) inode_table_blocks = 1;
    u64 inode_table_start = 1 + bitmap_blocks;
    u64 data_start = inode_table_start + inode_table_blocks;
    // Need room for the metadata prefix, at least one data block, and the backup
    // superblock in the last block.
    if (data_start + 1 >= total_blocks) return -1;

    u64 inode_count = inode_table_blocks * ADYTUMFS_INODES_PER_BLOCK;
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
        .free_blocks = total_blocks - (data_start + 1),
        .free_inodes = inode_count - 1,
        .generation = 1,
        .feature_compat = 0,
        .feature_incompat = 0,
        .feature_ro_compat = 0,
        .data_checksum_region = 0,
    };

    // A 4 KiB staging block is too big for a modest stack, and the format path is
    // single threaded, so it is static.
    static u8 block[ADYTUMFS_BLOCK_SIZE];

    adytumfs_super_pack(block, &super);
    if (adytumfs_block_write(device, 0, block) ||
        adytumfs_block_write(device, total_blocks - 1, block))
        return -1;

    // The metadata prefix [0, data_start) and the backup block are allocated; an
    // empty root directory owns no data blocks yet.
    for (u64 bitmap = 0; bitmap < bitmap_blocks; bitmap++) {
        for (u32 index = 0; index < ADYTUMFS_BLOCK_SIZE; index++) block[index] = 0;
        u64 base = bitmap * bits_per_block;
        for (u64 used = 0; used < data_start; used++)
            if (used >= base && used - base < bits_per_block)
                block[(used - base) / 8] |= (u8)(1u << ((used - base) % 8));
        u64 backup = total_blocks - 1;
        if (backup >= base && backup - base < bits_per_block)
            block[(backup - base) / 8] |= (u8)(1u << ((backup - base) % 8));
        if (adytumfs_block_write(device, super.block_bitmap_start + bitmap, block))
            return -1;
    }

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
        if (adytumfs_block_write(device, inode_table_start + table, block))
            return -1;
    }

    return block_cache_flush(device);
}
