#include "types.h"
#include "object.h"
#include "block.h"

int test_block64(void) {
    u32 objects = object_active_count();
    u32 devices = block_active_count();
    struct kernel_object *dev = block_create(64, 0);
    struct kernel_object *ro = block_create(8, BLOCK_FLAG_READ_ONLY);
    struct block_info info;
    u8 pattern[BLOCK_SECTOR_SIZE];
    u8 got[BLOCK_SECTOR_SIZE];
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++) {
        pattern[i] = (u8)(i * 3 + 1);
        got[i] = 0;
    }
    u64 id = 0;
    i32 status = 1;
    u32 transferred = 0;
    int valid = dev && ro && !block_info(dev, &info) &&
        info.sector_size == BLOCK_SECTOR_SIZE && info.sector_count == 64 &&
        !info.flags &&
        !block_submit(dev, BLOCK_OP_WRITE, 7, 1, pattern,
                      BLOCK_SECTOR_SIZE, &id) && id &&
        !block_collect(dev, id, &status, &transferred, 0, 0) &&
        !status && transferred == BLOCK_SECTOR_SIZE &&
        block_collect(dev, id, &status, &transferred, 0, 0) < 0 &&
        !block_submit(dev, BLOCK_OP_READ, 7, 1, got,
                      BLOCK_SECTOR_SIZE, &id) &&
        !block_collect(dev, id, &status, &transferred, 0, 0) && !status;
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++)
        if (got[i] != pattern[i]) valid = 0;
    u8 zero[BLOCK_SECTOR_SIZE];
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++) zero[i] = 0xFF;
    valid = valid &&
        !block_submit(dev, BLOCK_OP_READ, 0, 1, zero,
                      BLOCK_SECTOR_SIZE, &id) &&
        !block_collect(dev, id, &status, &transferred, 0, 0);
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++)
        if (zero[i]) valid = 0;
    valid = valid &&
        block_submit(dev, BLOCK_OP_WRITE, 64, 1, pattern,
                     BLOCK_SECTOR_SIZE, &id) < 0 &&
        block_submit(dev, BLOCK_OP_WRITE, 63, 2, pattern,
                     BLOCK_SECTOR_SIZE, &id) < 0 &&
        block_submit(dev, 0, 0, 1, pattern, BLOCK_SECTOR_SIZE, &id) < 0 &&
        block_submit(ro, BLOCK_OP_WRITE, 0, 1, pattern,
                     BLOCK_SECTOR_SIZE, &id) < 0 &&
        !block_submit(ro, BLOCK_OP_READ, 0, 1, got,
                      BLOCK_SECTOR_SIZE, &id) &&
        !block_collect(ro, id, &status, &transferred, 0, 0);
    u64 held[BLOCK_REQUEST_MAX];
    u32 posted = 0;
    while (posted < BLOCK_REQUEST_MAX) {
        if (block_submit(dev, BLOCK_OP_READ, 1, 1, got,
                         BLOCK_SECTOR_SIZE, &held[posted]))
            break;
        posted++;
    }
    valid = valid && posted == BLOCK_REQUEST_MAX &&
        block_submit(dev, BLOCK_OP_READ, 1, 1, got,
                     BLOCK_SECTOR_SIZE, &id) < 0 &&
        !block_collect(dev, held[0], &status, &transferred, 0, 0) &&
        !block_submit(dev, BLOCK_OP_READ, 1, 1, got,
                      BLOCK_SECTOR_SIZE, &id);
    for (u32 i = 1; i < posted; i++)
        if (block_collect(dev, held[i], &status, &transferred, 0, 0)) valid = 0;
    if (block_collect(dev, id, &status, &transferred, 0, 0)) valid = 0;
    valid = valid && !block_revoke(dev) &&
        block_submit(dev, BLOCK_OP_READ, 0, 1, got,
                     BLOCK_SECTOR_SIZE, &id) < 0 &&
        block_revoke(dev) < 0;
    struct kernel_object *async = block_create(16, BLOCK_FLAG_DEFER);
    u8 async_got[BLOCK_SECTOR_SIZE];
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++) async_got[i] = 0;
    valid = valid && async &&
        !block_submit(async, BLOCK_OP_WRITE, 2, 1, pattern,
                      BLOCK_SECTOR_SIZE, &id) &&
        block_collect(async, id, &status, &transferred, 0, 0) < 0 &&
        !block_service(async) &&
        !block_collect(async, id, &status, &transferred, 0, 0) &&
        !status && transferred == BLOCK_SECTOR_SIZE &&
        !block_submit(async, BLOCK_OP_READ, 2, 1, async_got,
                      BLOCK_SECTOR_SIZE, &id) &&
        block_collect(async, id, &status, &transferred,
                      async_got, BLOCK_SECTOR_SIZE) < 0 &&
        !block_io(async, BLOCK_OP_READ, 2, 1, async_got, BLOCK_SECTOR_SIZE);
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++)
        if (async_got[i] != pattern[i]) valid = 0;
    if (async) object_release(async);
    if (dev) object_release(dev);
    if (ro) object_release(ro);
    valid = valid && object_active_count() == objects &&
        block_active_count() == devices;
    return valid ? 0 : -1;
}
