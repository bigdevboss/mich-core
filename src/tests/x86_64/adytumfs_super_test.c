#include "types.h"
#include "adytumfs_format.h"

int test_adytumfs_super64(void) {
    // A 4 KiB block is too big for a modest stack, and this test is single
    // threaded, so the scratch block is static.
    static u8 block[ADYTUMFS_BLOCK_SIZE];
    struct adytumfs_superblock super = {
        .format_version = ADYTUMFS_FORMAT_VERSION,
        .block_size = ADYTUMFS_BLOCK_SIZE,
        .total_blocks = 1024,
        .block_bitmap_start = 1,
        .block_bitmap_blocks = 1,
        .inode_table_start = 2,
        .inode_table_blocks = 4,
        .inode_count = 64,
        .root_inode = ADYTUMFS_ROOT_INODE,
        .data_start = 6,
        .free_blocks = 1018,
        .free_inodes = 63,
        .generation = 1,
        .feature_compat = 0,
        .feature_incompat = 0,
        .feature_ro_compat = 0,
        .data_checksum_region = 0,
    };

    adytumfs_super_pack(block, &super);

    // The signature lands raw at the front so a foreign tool can spot the volume.
    int valid = block[0] == 'A' && block[7] == 'S' &&
        adytumfs_super_valid(&super, 1024) == 0 &&
        adytumfs_super_valid(&super, 512) != 0;

    struct adytumfs_superblock decoded;
    valid = valid && adytumfs_super_unpack(&decoded, block) == 0 &&
        decoded.format_version == super.format_version &&
        decoded.block_size == super.block_size &&
        decoded.total_blocks == super.total_blocks &&
        decoded.block_bitmap_start == super.block_bitmap_start &&
        decoded.block_bitmap_blocks == super.block_bitmap_blocks &&
        decoded.inode_table_start == super.inode_table_start &&
        decoded.inode_table_blocks == super.inode_table_blocks &&
        decoded.inode_count == super.inode_count &&
        decoded.root_inode == super.root_inode &&
        decoded.data_start == super.data_start &&
        decoded.free_blocks == super.free_blocks &&
        decoded.free_inodes == super.free_inodes &&
        decoded.generation == super.generation &&
        decoded.feature_incompat == super.feature_incompat &&
        decoded.data_checksum_region == super.data_checksum_region;

    // The checksum must catch a single flipped byte, and a broken signature
    // must be rejected before anything else.
    block[200] ^= 0x40u;
    valid = valid && adytumfs_super_unpack(&decoded, block) != 0;
    adytumfs_super_pack(block, &super);
    block[0] = 'X';
    valid = valid && adytumfs_super_unpack(&decoded, block) != 0;

    // Bounds validation rejects an out-of-range root inode and a region that is
    // not where the fixed order demands.
    struct adytumfs_superblock bad = super;
    bad.root_inode = super.inode_count;
    valid = valid && adytumfs_super_valid(&bad, 1024) != 0;
    bad = super;
    bad.inode_table_start = 3;
    valid = valid && adytumfs_super_valid(&bad, 1024) != 0;

    return valid ? 0 : -1;
}
