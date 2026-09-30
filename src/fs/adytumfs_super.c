#include "adytumfs_format.h"
#include "crc32c.h"

void adytumfs_super_pack(u8 *block, const struct adytumfs_superblock *super) {
    for (u32 index = 0; index < ADYTUMFS_BLOCK_SIZE; index++) block[index] = 0;
    static const char signature[] = ADYTUMFS_SIGNATURE;
    for (u32 index = 0; index < ADYTUMFS_SIGNATURE_SIZE; index++)
        block[index] = (u8)signature[index];
    adytumfs_write_le32(block + 8, super->format_version);
    adytumfs_write_le32(block + 12, super->block_size);
    adytumfs_write_le64(block + 16, super->total_blocks);
    adytumfs_write_le64(block + 24, super->block_bitmap_start);
    adytumfs_write_le64(block + 32, super->block_bitmap_blocks);
    adytumfs_write_le64(block + 40, super->inode_table_start);
    adytumfs_write_le64(block + 48, super->inode_table_blocks);
    adytumfs_write_le64(block + 56, super->inode_count);
    adytumfs_write_le64(block + 64, super->root_inode);
    adytumfs_write_le64(block + 72, super->data_start);
    adytumfs_write_le64(block + 80, super->free_blocks);
    adytumfs_write_le64(block + 88, super->free_inodes);
    adytumfs_write_le64(block + 96, super->generation);
    adytumfs_write_le64(block + 104, super->feature_compat);
    adytumfs_write_le64(block + 112, super->feature_incompat);
    adytumfs_write_le64(block + 120, super->feature_ro_compat);
    adytumfs_write_le64(block + 128, super->data_checksum_region);
    adytumfs_write_le32(block + ADYTUMFS_SUPER_CHECKSUM_OFFSET,
                        crc32c(block, ADYTUMFS_SUPER_CHECKSUM_OFFSET));
}

int adytumfs_super_unpack(struct adytumfs_superblock *super, const u8 *block) {
    static const char signature[] = ADYTUMFS_SIGNATURE;
    for (u32 index = 0; index < ADYTUMFS_SIGNATURE_SIZE; index++)
        if (block[index] != (u8)signature[index]) return -1;
    if (adytumfs_read_le32(block + ADYTUMFS_SUPER_CHECKSUM_OFFSET) !=
        crc32c(block, ADYTUMFS_SUPER_CHECKSUM_OFFSET))
        return -1;
    super->format_version = adytumfs_read_le32(block + 8);
    super->block_size = adytumfs_read_le32(block + 12);
    if (super->format_version != ADYTUMFS_FORMAT_VERSION ||
        super->block_size != ADYTUMFS_BLOCK_SIZE)
        return -1;
    super->total_blocks = adytumfs_read_le64(block + 16);
    super->block_bitmap_start = adytumfs_read_le64(block + 24);
    super->block_bitmap_blocks = adytumfs_read_le64(block + 32);
    super->inode_table_start = adytumfs_read_le64(block + 40);
    super->inode_table_blocks = adytumfs_read_le64(block + 48);
    super->inode_count = adytumfs_read_le64(block + 56);
    super->root_inode = adytumfs_read_le64(block + 64);
    super->data_start = adytumfs_read_le64(block + 72);
    super->free_blocks = adytumfs_read_le64(block + 80);
    super->free_inodes = adytumfs_read_le64(block + 88);
    super->generation = adytumfs_read_le64(block + 96);
    super->feature_compat = adytumfs_read_le64(block + 104);
    super->feature_incompat = adytumfs_read_le64(block + 112);
    super->feature_ro_compat = adytumfs_read_le64(block + 120);
    super->data_checksum_region = adytumfs_read_le64(block + 128);
    return 0;
}

int adytumfs_super_valid(const struct adytumfs_superblock *super,
                         u64 device_blocks) {
    // An unknown incompatible feature means the on-disk layout is one we cannot
    // safely interpret, so refuse the volume rather than guess.
    if (super->feature_incompat) return -1;
    if (super->block_size != ADYTUMFS_BLOCK_SIZE) return -1;
    if (super->total_blocks > device_blocks) return -1;

    // Regions sit in the fixed order block 0 (super), bitmap, inode table, then
    // data, up to total_blocks. Each bound is checked with subtraction so a
    // crafted size cannot overflow the addition.
    if (super->block_bitmap_start != 1) return -1;
    if (super->block_bitmap_blocks == 0 ||
        super->block_bitmap_blocks > super->total_blocks - 1)
        return -1;
    if (super->inode_table_start !=
        super->block_bitmap_start + super->block_bitmap_blocks)
        return -1;
    if (super->inode_table_blocks == 0 ||
        super->inode_table_start > super->total_blocks ||
        super->inode_table_blocks > super->total_blocks - super->inode_table_start)
        return -1;
    if (super->data_start !=
        super->inode_table_start + super->inode_table_blocks)
        return -1;
    if (super->data_start >= super->total_blocks) return -1;

    if (super->inode_count == 0 ||
        super->inode_count >
        super->inode_table_blocks * ADYTUMFS_INODES_PER_BLOCK)
        return -1;
    if (super->root_inode == 0 || super->root_inode >= super->inode_count)
        return -1;
    if (super->data_checksum_region &&
        (super->data_checksum_region < super->data_start ||
         super->data_checksum_region >= super->total_blocks))
        return -1;
    return 0;
}

// Rewrite the primary superblock from the in-memory copy so the allocation
// counters on disk match what the allocator has handed out. The backup copy
// stays a format time snapshot until the durability work turns both copies
// into a generation pair.
int adytumfs_super_sync(struct kernel_object *device,
                        const struct adytumfs_superblock *super) {
    if (!device || !super) return -1;
    // A 4 KiB staging block is too large for a modest stack, and the allocator
    // paths are single threaded, so the staging is static.
    static u8 block[ADYTUMFS_BLOCK_SIZE];
    adytumfs_super_pack(block, super);
    return adytumfs_block_write(device, 0, block);
}
