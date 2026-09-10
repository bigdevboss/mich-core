#include "types.h"
#include "object.h"
#include "block.h"
#include "cache.h"

int test_cache64(void) {
    u32 objects = object_active_count();
    u32 devices = block_active_count();
    struct kernel_object *dev = block_create(64, 0);
    u8 pattern[BLOCK_SECTOR_SIZE];
    u8 got[BLOCK_SECTOR_SIZE];
    u8 other[BLOCK_SECTOR_SIZE];
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++) {
        pattern[i] = (u8)(0xC0 + (i & 0x3F));
        got[i] = 0;
        other[i] = (u8)(0x11 + i);
    }
    int valid = dev &&
        !block_cache_write(dev, 9, pattern, 1) &&
        block_cache_dirty_count() &&
        !block_cache_read(dev, 9, got, 1);
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++)
        if (got[i] != pattern[i]) valid = 0;
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++) got[i] = 0;
    valid = valid && !block_cache_flush(dev) && !block_cache_dirty_count() &&
        !block_io(dev, BLOCK_OP_READ, 9, 1, got, BLOCK_SECTOR_SIZE);
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++)
        if (got[i] != pattern[i]) valid = 0;
    valid = valid && !block_cache_write(dev, 0, other, 1) &&
        !block_cache_read(dev, 9, got, 1);
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++)
        if (got[i] != pattern[i]) valid = 0;
    if (dev) {
        block_cache_drop_device((u32)dev->value);
        object_release(dev);
    }
    valid = valid && object_active_count() == objects &&
        block_active_count() == devices && !block_cache_dirty_count();
    return valid ? 0 : -1;
}
