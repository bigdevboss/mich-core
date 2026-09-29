#include "types.h"
#include "object.h"
#include "block.h"
#include "adytumfs_format.h"

static int bitmap_used(const u8 *bitmap, u64 block) {
    return (bitmap[block / 8] >> (block % 8)) & 1;
}

int test_adytumfs_alloc64(void) {
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

    u64 free0 = super.free_blocks;
    u64 first = 0;
    u64 second = 0;
    u64 reused = 0;

    // First-fit places the first run at the start of the data region and the
    // second immediately after it.
    valid = valid && adytumfs_alloc_run(dev, &super, 3, &first) == 0 &&
        first == super.data_start &&
        adytumfs_alloc_run(dev, &super, 2, &second) == 0 &&
        second == first + 3 &&
        super.free_blocks == free0 - 5;

    valid = valid &&
        adytumfs_block_read(dev, super.block_bitmap_start, block) == 0 &&
        bitmap_used(block, first) && bitmap_used(block, first + 2) &&
        bitmap_used(block, second) && bitmap_used(block, second + 1) &&
        !bitmap_used(block, second + 2);

    // Freeing the first run returns its blocks, and the next allocation reuses
    // exactly that hole.
    valid = valid && adytumfs_free_run(dev, &super, first, 3) == 0 &&
        super.free_blocks == free0 - 2 &&
        adytumfs_block_read(dev, super.block_bitmap_start, block) == 0 &&
        !bitmap_used(block, first) &&
        adytumfs_alloc_run(dev, &super, 3, &reused) == 0 &&
        reused == first && super.free_blocks == free0 - 5;

    // A run larger than the free space fails, and metadata blocks cannot be
    // freed.
    u64 overflow = 0;
    valid = valid &&
        adytumfs_alloc_run(dev, &super, super.total_blocks, &overflow) != 0 &&
        adytumfs_free_run(dev, &super, 0, 1) != 0;

    if (dev) object_release(dev);
    valid = valid && object_active_count() == objects &&
        block_active_count() == devices;
    return valid ? 0 : -1;
}
