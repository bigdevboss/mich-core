#include "types.h"
#include "object.h"
#include "block.h"
#include "adytumfs_format.h"

int test_adytumfs_inode64(void) {
    u32 objects = object_active_count();
    u32 devices = block_active_count();
    struct kernel_object *dev = block_create(512, 0);   // 64 filesystem blocks

    // A 4 KiB block is too big for a modest stack, and the test is single
    // threaded, so the scratch block is static.
    static u8 block[ADYTUMFS_BLOCK_SIZE];
    struct adytumfs_superblock super;
    int valid = dev && adytumfs_make(dev) == 0 &&
        adytumfs_block_read(dev, 0, block) == 0 &&
        adytumfs_super_unpack(&super, block) == 0;

    // The root inode reads back through the inode-table path as a directory.
    struct adytumfs_inode root;
    valid = valid &&
        adytumfs_inode_read(dev, &super, ADYTUMFS_ROOT_INODE, &root) == 0 &&
        (root.mode & ADYTUMFS_MODE_DIR);

    u64 free0 = super.free_inodes;

    // Allocate a regular-file inode, then round-trip its fields.
    u64 number = 0;
    valid = valid &&
        adytumfs_inode_alloc(dev, &super, ADYTUMFS_MODE_REG | 0644u, &number) == 0 &&
        number >= 1 && number < super.inode_count &&
        super.free_inodes == free0 - 1;
    struct adytumfs_inode file;
    valid = valid && adytumfs_inode_read(dev, &super, number, &file) == 0 &&
        (file.mode & ADYTUMFS_MODE_REG) && file.links == 1 &&
        file.generation >= 1;
    u64 generation = file.generation;

    file.size = 12345;
    file.direct[0].start_block = super.data_start;
    file.direct[0].length = 2;
    struct adytumfs_inode again;
    valid = valid && adytumfs_inode_write(dev, &super, number, &file) == 0 &&
        adytumfs_inode_read(dev, &super, number, &again) == 0 &&
        again.size == 12345 &&
        again.direct[0].start_block == super.data_start &&
        again.direct[0].length == 2;

    // Freeing returns the slot; the next allocation reuses it with a higher
    // generation.
    u64 reused = 0;
    struct adytumfs_inode fresh;
    valid = valid && adytumfs_inode_free(dev, &super, number) == 0 &&
        super.free_inodes == free0 &&
        adytumfs_inode_alloc(dev, &super, ADYTUMFS_MODE_REG | 0600u, &reused) == 0 &&
        reused == number &&
        adytumfs_inode_read(dev, &super, reused, &fresh) == 0 &&
        fresh.generation == generation + 1;

    // A flipped byte in an in-use inode slot is caught by its checksum.
    u64 table = super.inode_table_start + reused / ADYTUMFS_INODES_PER_BLOCK;
    u32 slot = (u32)(reused % ADYTUMFS_INODES_PER_BLOCK) * ADYTUMFS_INODE_SIZE;
    struct adytumfs_inode corrupt;
    valid = valid && adytumfs_block_read(dev, table, block) == 0;
    block[slot + 16] ^= 0x20u;
    valid = valid && adytumfs_block_write(dev, table, block) == 0 &&
        adytumfs_inode_read(dev, &super, reused, &corrupt) != 0;

    // Out-of-range inode numbers are rejected.
    valid = valid && adytumfs_inode_read(dev, &super, 0, &corrupt) != 0 &&
        adytumfs_inode_read(dev, &super, super.inode_count, &corrupt) != 0;

    if (dev) object_release(dev);
    valid = valid && object_active_count() == objects &&
        block_active_count() == devices;
    return valid ? 0 : -1;
}
