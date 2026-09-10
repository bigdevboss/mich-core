#include "cache.h"
#include "block.h"

#define CACHE_EMPTY 0
#define CACHE_CLEAN 1
#define CACHE_DIRTY 2

struct cache_entry {
    struct kernel_object *device;
    u32 device_id;
    u32 lba;
    u32 sectors;
    u32 state;
    u8 data[BLOCK_SECTOR_SIZE * BLOCK_CACHE_SECTORS];
};

static struct cache_entry entries[BLOCK_CACHE_MAX];
static u32 clock;

static void copy_bytes(u8 *dst, const u8 *src, u32 n) {
    for (u32 i = 0; i < n; i++) dst[i] = src[i];
}

static u32 page_lba(u32 lba) {
    return lba - (lba % BLOCK_CACHE_SECTORS);
}

static struct cache_entry *lookup(u32 device_id, u32 lba) {
    u32 page = page_lba(lba);
    for (u32 i = 0; i < BLOCK_CACHE_MAX; i++)
        if (entries[i].state != CACHE_EMPTY &&
            entries[i].device_id == device_id && entries[i].lba == page)
            return &entries[i];
    return 0;
}

static int writeback(struct cache_entry *entry) {
    if (entry->state != CACHE_DIRTY) return 0;
    if (!entry->device) return -1;
    for (u32 s = 0; s < entry->sectors; s++)
        if (block_io(entry->device, BLOCK_OP_WRITE, entry->lba + s, 1,
                     entry->data + s * BLOCK_SECTOR_SIZE, BLOCK_SECTOR_SIZE))
            return -1;
    entry->state = CACHE_CLEAN;
    return 0;
}

static struct cache_entry *evict(void) {
    for (u32 n = 0; n < BLOCK_CACHE_MAX * 2; n++) {
        struct cache_entry *entry = &entries[clock];
        clock++;
        if (clock >= BLOCK_CACHE_MAX) clock = 0;
        if (entry->state == CACHE_EMPTY) return entry;
        if (entry->state == CACHE_CLEAN) {
            entry->state = CACHE_EMPTY;
            entry->device = 0;
            return entry;
        }
        if (entry->state == CACHE_DIRTY && !writeback(entry)) {
            entry->state = CACHE_EMPTY;
            entry->device = 0;
            return entry;
        }
    }
    return 0;
}

static struct cache_entry *fill(struct kernel_object *device, u32 lba) {
    u32 id = (u32)device->value;
    u32 page = page_lba(lba);
    struct cache_entry *entry = lookup(id, page);
    if (entry) {
        entry->device = device;
        return entry;
    }
    entry = evict();
    if (!entry) return 0;
    struct block_info info;
    if (block_info(device, &info)) return 0;
    u32 sectors = BLOCK_CACHE_SECTORS;
    if (page >= info.sector_count) return 0;
    if (sectors > info.sector_count - page)
        sectors = info.sector_count - page;
    for (u32 i = 0; i < sizeof(entry->data); i++) entry->data[i] = 0;
    for (u32 s = 0; s < sectors; s++)
        if (block_io(device, BLOCK_OP_READ, page + s, 1,
                     entry->data + s * BLOCK_SECTOR_SIZE, BLOCK_SECTOR_SIZE))
            return 0;
    entry->device = device;
    entry->device_id = id;
    entry->lba = page;
    entry->sectors = sectors;
    entry->state = CACHE_CLEAN;
    return entry;
}

void block_cache_init(void) {
    clock = 0;
    for (u32 i = 0; i < BLOCK_CACHE_MAX; i++) {
        entries[i].device = 0;
        entries[i].device_id = 0;
        entries[i].lba = 0;
        entries[i].sectors = 0;
        entries[i].state = CACHE_EMPTY;
    }
}

int block_cache_read(struct kernel_object *device, u32 lba,
                     void *buffer, u32 sectors) {
    if (!device || !buffer || !sectors) return -1;
    u32 done = 0;
    while (done < sectors) {
        struct cache_entry *entry = fill(device, lba + done);
        if (!entry) return -1;
        u32 off = (lba + done) - entry->lba;
        u32 n = BLOCK_CACHE_SECTORS - off;
        if (n > sectors - done) n = sectors - done;
        copy_bytes((u8 *)buffer + done * BLOCK_SECTOR_SIZE,
                   entry->data + off * BLOCK_SECTOR_SIZE,
                   n * BLOCK_SECTOR_SIZE);
        done += n;
    }
    return 0;
}

int block_cache_write(struct kernel_object *device, u32 lba,
                      const void *buffer, u32 sectors) {
    if (!device || !buffer || !sectors) return -1;
    u32 done = 0;
    while (done < sectors) {
        struct cache_entry *entry = fill(device, lba + done);
        if (!entry) return -1;
        u32 off = (lba + done) - entry->lba;
        u32 n = BLOCK_CACHE_SECTORS - off;
        if (n > sectors - done) n = sectors - done;
        copy_bytes(entry->data + off * BLOCK_SECTOR_SIZE,
                   (const u8 *)buffer + done * BLOCK_SECTOR_SIZE,
                   n * BLOCK_SECTOR_SIZE);
        entry->state = CACHE_DIRTY;
        done += n;
    }
    return 0;
}

int block_cache_flush(struct kernel_object *device) {
    if (!device) return -1;
    u32 id = (u32)device->value;
    for (u32 i = 0; i < BLOCK_CACHE_MAX; i++) {
        if (entries[i].device_id != id) continue;
        entries[i].device = device;
        if (writeback(&entries[i])) return -1;
    }
    return 0;
}

void block_cache_drop_device(u32 device) {
    for (u32 i = 0; i < BLOCK_CACHE_MAX; i++) {
        if (entries[i].device_id != device) continue;
        entries[i].device = 0;
        entries[i].device_id = 0;
        entries[i].lba = 0;
        entries[i].sectors = 0;
        entries[i].state = CACHE_EMPTY;
    }
}

u32 block_cache_dirty_count(void) {
    u32 n = 0;
    for (u32 i = 0; i < BLOCK_CACHE_MAX; i++)
        if (entries[i].state == CACHE_DIRTY) n++;
    return n;
}
