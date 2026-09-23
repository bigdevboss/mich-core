#include "blockfs.h"
#include "vfs.h"
#include "block.h"
#include "cache.h"
#include "resource.h"

#define BLOCKFS_INODE_BYTES 64
#define BLOCKFS_INODES_PER_SECTOR (BLOCK_SECTOR_SIZE / BLOCKFS_INODE_BYTES)
#define BLOCKFS_MOUNT_MAX 8

struct blockfs_super {
    u32 magic;
    u32 inode_count;
    u32 inode_lba;
    u32 data_lba;
    u32 data_sectors;
    u32 next_data;
};

struct blockfs_inode {
    u32 used;
    u32 type;
    u32 size;
    u32 start_lba;
    u32 sectors;
    u32 parent;
    char name[32];
    u32 mode;
    u32 reserved;
};

struct blockfs_cache {
    struct kernel_object *pages;
    u64 present;
    u64 dirty;
    u32 inode;
    u32 active;
};

struct blockfs_mount {
    struct kernel_object *device;
    struct blockfs_cache cache[BLOCKFS_INODE_MAX];
    u32 inode_count;
    u32 inode_lba;
    u32 data_lba;
    u32 data_sectors;
    u32 next_data;
    u32 active;
};

static struct blockfs_mount mounts[BLOCKFS_MOUNT_MAX];

static void copy_bytes(u8 *dst, const u8 *src, u32 n) {
    for (u32 i = 0; i < n; i++) dst[i] = src[i];
}

static void inode_pack(u8 *raw, const struct blockfs_inode *in) {
    u32 *w = (u32 *)raw;
    w[0] = in->used;
    w[1] = in->type;
    w[2] = in->size;
    w[3] = in->start_lba;
    w[4] = in->sectors;
    w[5] = in->parent;
    copy_bytes(raw + 24, (const u8 *)in->name, 32);
    w[14] = in->mode;
    w[15] = in->reserved;
}

static void inode_unpack(struct blockfs_inode *in, const u8 *raw) {
    const u32 *w = (const u32 *)raw;
    in->used = w[0];
    in->type = w[1];
    in->size = w[2];
    in->start_lba = w[3];
    in->sectors = w[4];
    in->parent = w[5];
    copy_bytes((u8 *)in->name, raw + 24, 32);
    in->mode = w[14];
    in->reserved = w[15];
}

static struct blockfs_mount *mount_at(u32 mount) {
    if (!mount || mount >= BLOCKFS_MOUNT_MAX) return 0;
    struct blockfs_mount *m = &mounts[mount];
    return m->active ? m : 0;
}

static int load_super(struct kernel_object *device, struct blockfs_super *super) {
    u8 sector[BLOCK_SECTOR_SIZE];
    if (block_cache_read(device, 0, sector, 1)) return -1;
    const u32 *w = (const u32 *)sector;
    super->magic = w[0];
    super->inode_count = w[1];
    super->inode_lba = w[2];
    super->data_lba = w[3];
    super->data_sectors = w[4];
    super->next_data = w[5];
    if (super->magic != BLOCKFS_MAGIC ||
        !super->inode_count || super->inode_count > BLOCKFS_INODE_MAX ||
        super->inode_lba != 1 || super->data_lba < 2)
        return -1;
    return 0;
}

static int store_super(struct kernel_object *device,
                       const struct blockfs_super *super) {
    u8 sector[BLOCK_SECTOR_SIZE];
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++) sector[i] = 0;
    u32 *w = (u32 *)sector;
    w[0] = super->magic;
    w[1] = super->inode_count;
    w[2] = super->inode_lba;
    w[3] = super->data_lba;
    w[4] = super->data_sectors;
    w[5] = super->next_data;
    return block_cache_write(device, 0, sector, 1) ||
           block_cache_flush(device);
}

static int persist_super(struct blockfs_mount *m) {
    struct blockfs_super super;
    super.magic = BLOCKFS_MAGIC;
    super.inode_count = m->inode_count;
    super.inode_lba = m->inode_lba;
    super.data_lba = m->data_lba;
    super.data_sectors = m->data_sectors;
    super.next_data = m->next_data;
    return store_super(m->device, &super);
}

static int load_inode(struct blockfs_mount *m, u32 inode,
                      struct blockfs_inode *out) {
    if (!m || inode >= m->inode_count) return -1;
    u8 sector[BLOCK_SECTOR_SIZE];
    u32 lba = m->inode_lba + inode / BLOCKFS_INODES_PER_SECTOR;
    u32 off = (inode % BLOCKFS_INODES_PER_SECTOR) * BLOCKFS_INODE_BYTES;
    if (block_cache_read(m->device, lba, sector, 1)) return -1;
    inode_unpack(out, sector + off);
    return 0;
}

static int store_inode(struct blockfs_mount *m, u32 inode,
                       const struct blockfs_inode *in) {
    if (!m || inode >= m->inode_count) return -1;
    u8 sector[BLOCK_SECTOR_SIZE];
    u32 lba = m->inode_lba + inode / BLOCKFS_INODES_PER_SECTOR;
    u32 off = (inode % BLOCKFS_INODES_PER_SECTOR) * BLOCKFS_INODE_BYTES;
    if (block_cache_read(m->device, lba, sector, 1)) return -1;
    inode_pack(sector + off, in);
    return block_cache_write(m->device, lba, sector, 1) ||
           block_cache_flush(m->device);
}

static int ensure_extent(struct blockfs_mount *m, struct blockfs_inode *in) {
    if (in->start_lba) return 0;
    if (m->next_data + BLOCKFS_FILE_SECTORS > m->data_lba + m->data_sectors)
        return -1;
    in->start_lba = m->next_data;
    in->sectors = BLOCKFS_FILE_SECTORS;
    m->next_data += BLOCKFS_FILE_SECTORS;
    return persist_super(m);
}

static struct blockfs_cache *cache_slot(struct blockfs_mount *m, u32 inode) {
    if (!m || inode >= m->inode_count) return 0;
    struct blockfs_cache *slot = &m->cache[inode];
    if (!slot->active) return 0;
    struct page_resource *resource =
        slot->pages ? page_resource_get(slot->pages) : 0;
    if (!resource || resource->revoked) {
        slot->pages = 0;
        slot->present = 0;
        slot->dirty = 0;
        slot->active = 0;
        return 0;
    }
    return slot;
}

// One submit is bounded by BLOCK_IO_SG_SECTORS_MAX sectors, not by the SG
// entry count, so a transfer is chunked at eight pages even though a page
// resource holds up to sixty four.
#define BLOCKFS_SG_PAGES (BLOCK_IO_SG_SECTORS_MAX / (4096u / BLOCK_SECTOR_SIZE))

static int page_transfer(struct blockfs_mount *m, struct blockfs_cache *slot,
                         u32 start_lba, u32 first, u32 count, u32 op) {
    if (!count || count > BLOCKFS_SG_PAGES) return -1;
    struct kernel_object *pages[BLOCKFS_SG_PAGES];
    u32 indices[BLOCKFS_SG_PAGES];
    u32 offsets[BLOCKFS_SG_PAGES];
    u32 lengths[BLOCKFS_SG_PAGES];
    for (u32 index = 0; index < count; index++) {
        pages[index] = slot->pages;
        indices[index] = first + index;
        offsets[index] = 0;
        lengths[index] = 4096;
    }
    struct kernel_object *sg =
        sg_resource_create(pages, indices, offsets, lengths, count);
    if (!sg) return -1;
    u32 lba = start_lba + first * (4096u / BLOCK_SECTOR_SIZE);
    u32 sectors = count * (4096u / BLOCK_SECTOR_SIZE);
    u64 id = 0;
    i32 status = -1;
    u32 transferred = 0;
    int result = -1;
    if (block_submit_sg(m->device, op, lba, sectors, sg, &id)) {
        // Ramdisk-backed devices carry no scatter-gather transport, so the
        // page still has to move one page at a time through the copy path.
        struct page_resource *resource = page_resource_get(slot->pages);
        result = 0;
        for (u32 index = 0; index < count && !result; index++) {
            u8 *bytes = resource ?
                (u8 *)(uptr_t)resource->physical[first + index] : 0;
            if (!bytes) {
                result = -1;
                break;
            }
            for (u32 sector = 0;
                 sector < 4096u / BLOCK_SECTOR_SIZE && !result; sector++)
                result = block_io(m->device, op,
                                  lba + index * (4096u / BLOCK_SECTOR_SIZE) +
                                      sector,
                                  1, bytes + sector * BLOCK_SECTOR_SIZE,
                                  BLOCK_SECTOR_SIZE);
        }
        sg_resource_revoke(sg);
        object_release(sg);
        return result;
    }
    {
        if (block_collect(m->device, id, &status, &transferred, 0, 0))
            for (u32 spin = 0; spin < 1000000; spin++) {
                block_service(m->device);
                if (!block_collect(m->device, id, &status, &transferred, 0, 0))
                    break;
                __asm__ volatile("pause");
            }
        if (!status && transferred == sectors * BLOCK_SECTOR_SIZE) result = 0;
    }
    sg_resource_revoke(sg);
    object_release(sg);
    return result;
}

int blockfs_pages_attach(u32 mount, u32 inode, struct kernel_object *pages) {
    struct blockfs_mount *m = mount_at(mount);
    struct blockfs_inode in;
    if (!m || !pages || inode >= m->inode_count || m->cache[inode].active ||
        load_inode(m, inode, &in) || !in.used || in.type != VFS_NODE_REGULAR)
        return -1;
    struct page_resource *resource = page_resource_get(pages);
    if (!resource || resource->pages > BLOCKFS_FILE_PAGES) return -1;
    struct blockfs_cache *slot = &m->cache[inode];
    slot->pages = pages;
    slot->present = 0;
    slot->dirty = 0;
    slot->inode = inode;
    slot->active = 1;
    return 0;
}

int blockfs_pages_fault(u32 mount, u32 inode, u32 page) {
    struct blockfs_mount *m = mount_at(mount);
    struct blockfs_cache *slot = cache_slot(m, inode);
    struct blockfs_inode in;
    if (!slot || page >= BLOCKFS_FILE_PAGES ||
        load_inode(m, inode, &in) || !in.used || in.type != VFS_NODE_REGULAR)
        return -1;
    if (slot->present & (1ull << page)) return 0;
    struct page_resource *resource = page_resource_get(slot->pages);
    if (!resource || page >= resource->pages) return -1;
    u8 *bytes = (u8 *)(uptr_t)resource->physical[page];
    if (!bytes) return -1;
    if (!in.start_lba || page * 4096u >= in.size) {
        for (u32 index = 0; index < 4096; index++) bytes[index] = 0;
        slot->present |= 1ull << page;
        return 0;
    }
    if (page_transfer(m, slot, in.start_lba, page, 1, BLOCK_OP_READ))
        return -1;
    slot->present |= 1ull << page;
    return 0;
}

int blockfs_pages_dirty(u32 mount, u32 inode, u32 page, u32 size) {
    struct blockfs_mount *m = mount_at(mount);
    struct blockfs_cache *slot = cache_slot(m, inode);
    struct blockfs_inode in;
    if (!slot || page >= BLOCKFS_FILE_PAGES || size > BLOCKFS_FILE_SIZE_MAX ||
        load_inode(m, inode, &in) || !in.used || in.type != VFS_NODE_REGULAR)
        return -1;
    slot->present |= 1ull << page;
    slot->dirty |= 1ull << page;
    if (size > in.size) {
        in.size = size;
        if (store_inode(m, inode, &in)) return -1;
    }
    return 0;
}

int blockfs_pages_sync(u32 mount, u32 inode) {
    struct blockfs_mount *m = mount_at(mount);
    struct blockfs_cache *slot = cache_slot(m, inode);
    struct blockfs_inode in;
    if (!slot) return 0;
    if (load_inode(m, inode, &in) || !in.used ||
        in.type != VFS_NODE_REGULAR)
        return -1;
    if (!slot->dirty) return 0;
    if (ensure_extent(m, &in) || store_inode(m, inode, &in)) return -1;
    u32 page = 0;
    while (page < BLOCKFS_FILE_PAGES) {
        if (!(slot->dirty & (1ull << page))) {
            page++;
            continue;
        }
        u32 count = 0;
        while (count < BLOCKFS_SG_PAGES && page + count < BLOCKFS_FILE_PAGES &&
               (slot->dirty & (1ull << (page + count))))
            count++;
        if (page_transfer(m, slot, in.start_lba, page, count,
                          BLOCK_OP_WRITE))
            return -1;
        for (u32 index = 0; index < count; index++)
            slot->dirty &= ~(1ull << (page + index));
        page += count;
    }
    return 0;
}

void blockfs_pages_detach(u32 mount, u32 inode) {
    struct blockfs_mount *m = mount_at(mount);
    if (!m || inode >= m->inode_count) return;
    struct blockfs_cache *slot = &m->cache[inode];
    slot->pages = 0;
    slot->present = 0;
    slot->dirty = 0;
    slot->inode = 0;
    slot->active = 0;
}

int blockfs_format(struct kernel_object *device) {
    struct block_info info;
    if (!device || block_info(device, &info) ||
        info.sector_count < 16 || (info.flags & BLOCK_FLAG_READ_ONLY))
        return -1;
    u32 inode_sectors = (BLOCKFS_INODE_MAX + BLOCKFS_INODES_PER_SECTOR - 1) /
                        BLOCKFS_INODES_PER_SECTOR;
    struct blockfs_super super;
    super.magic = BLOCKFS_MAGIC;
    super.inode_count = BLOCKFS_INODE_MAX;
    super.inode_lba = 1;
    super.data_lba = 1 + inode_sectors;
    super.data_sectors = info.sector_count - super.data_lba;
    super.next_data = super.data_lba;
    if (store_super(device, &super)) return -1;
    u8 sector[BLOCK_SECTOR_SIZE];
    for (u32 i = 0; i < BLOCK_SECTOR_SIZE; i++) sector[i] = 0;
    for (u32 s = 0; s < inode_sectors; s++)
        if (block_cache_write(device, super.inode_lba + s, sector, 1))
            return -1;
    struct blockfs_inode root;
    for (u32 i = 0; i < 32; i++) root.name[i] = 0;
    root.used = 1;
    root.type = VFS_NODE_DIRECTORY;
    root.size = 0;
    root.start_lba = 0;
    root.sectors = 0;
    root.parent = 0;
    root.mode = VFS_MODE_DIRECTORY_DEFAULT;
    root.reserved = 0;
    inode_pack(sector, &root);
    return block_cache_write(device, super.inode_lba, sector, 1) ||
           block_cache_flush(device);
}

int blockfs_attach(u32 mount, struct kernel_object *device) {
    struct blockfs_super super;
    if (mount_at(mount) || !device || load_super(device, &super)) return -1;
    if (object_retain(device)) return -1;
    struct blockfs_mount *m = &mounts[mount];
    for (u32 inode = 0; inode < BLOCKFS_INODE_MAX; inode++) {
        m->cache[inode].pages = 0;
        m->cache[inode].present = 0;
        m->cache[inode].dirty = 0;
        m->cache[inode].inode = 0;
        m->cache[inode].active = 0;
    }
    m->device = device;
    m->inode_count = super.inode_count;
    m->inode_lba = super.inode_lba;
    m->data_lba = super.data_lba;
    m->data_sectors = super.data_sectors;
    m->next_data = super.next_data;
    m->active = 1;
    return 0;
}

void blockfs_detach(u32 mount) {
    struct blockfs_mount *m = mount_at(mount);
    if (!m) return;
    if (m->device) {
        for (u32 inode = 0; inode < m->inode_count; inode++) {
            if (!m->cache[inode].active) continue;
            blockfs_pages_sync(mount, inode);
            blockfs_pages_detach(mount, inode);
        }
        block_cache_flush(m->device);
        object_release(m->device);
    }
    m->device = 0;
    m->inode_count = 0;
    m->active = 0;
}

u32 blockfs_inode_count(u32 mount) {
    struct blockfs_mount *m = mount_at(mount);
    return m ? m->inode_count : 0;
}

int blockfs_inode_get(u32 mount, u32 inode, u32 *used, u32 *type, u32 *size,
                      u32 *parent, u32 *mode, char *name) {
    struct blockfs_mount *m = mount_at(mount);
    struct blockfs_inode in;
    if (!m || load_inode(m, inode, &in)) return -1;
    if (used) *used = in.used;
    if (type) *type = in.type;
    if (size) *size = in.size;
    if (parent) *parent = in.parent;
    if (mode) *mode = in.mode;
    if (name)
        for (u32 i = 0; i < 32; i++) name[i] = in.name[i];
    return 0;
}

int blockfs_inode_create(u32 mount, const char *name, u32 parent, u32 type,
                         u32 mode, u32 *inode) {
    struct blockfs_mount *m = mount_at(mount);
    if (!m || !name || !name[0] || !inode || parent >= m->inode_count ||
        (type != VFS_NODE_REGULAR && type != VFS_NODE_DIRECTORY) ||
        (mode & ~VFS_MODE_MASK))
        return -1;
    struct blockfs_inode parent_inode;
    if (load_inode(m, parent, &parent_inode) || !parent_inode.used ||
        parent_inode.type != VFS_NODE_DIRECTORY)
        return -1;
    for (u32 i = 1; i < m->inode_count; i++) {
        struct blockfs_inode in;
        if (load_inode(m, i, &in)) return -1;
        if (in.used) continue;
        for (u32 n = 0; n < 32; n++) in.name[n] = 0;
        for (u32 n = 0; name[n] && n < 31; n++) in.name[n] = name[n];
        in.used = 1;
        in.type = type;
        in.size = 0;
        in.start_lba = 0;
        in.sectors = 0;
        in.parent = parent;
        in.mode = mode;
        in.reserved = 0;
        if (store_inode(m, i, &in)) return -1;
        *inode = i;
        return 0;
    }
    return -1;
}

int blockfs_inode_remove(u32 mount, u32 inode) {
    struct blockfs_mount *m = mount_at(mount);
    struct blockfs_inode in;
    if (!m || !inode || load_inode(m, inode, &in) || !in.used) return -1;
    blockfs_pages_detach(mount, inode);
    in.used = 0;
    in.size = 0;
    in.start_lba = 0;
    in.sectors = 0;
    in.parent = 0;
    for (u32 n = 0; n < 32; n++) in.name[n] = 0;
    in.mode = 0;
    return store_inode(m, inode, &in);
}

int blockfs_read(u32 mount, u32 inode, u32 offset, void *buffer,
                 u32 length, u32 *transferred) {
    struct blockfs_mount *m = mount_at(mount);
    struct blockfs_inode in;
    if (!m || !buffer || !transferred || load_inode(m, inode, &in) ||
        !in.used || in.type != VFS_NODE_REGULAR)
        return -1;
    if (offset >= in.size) {
        *transferred = 0;
        return 0;
    }
    u32 count = in.size - offset;
    if (count > length) count = length;
    u32 done = 0;
    while (done < count) {
        u32 pos = offset + done;
        u32 lba = in.start_lba + pos / BLOCK_SECTOR_SIZE;
        u32 skip = pos % BLOCK_SECTOR_SIZE;
        u32 n = BLOCK_SECTOR_SIZE - skip;
        if (n > count - done) n = count - done;
        u8 sector[BLOCK_SECTOR_SIZE];
        if (!in.start_lba ||
            block_cache_read(m->device, lba, sector, 1))
            return -1;
        copy_bytes((u8 *)buffer + done, sector + skip, n);
        done += n;
    }
    *transferred = done;
    return 0;
}

int blockfs_write(u32 mount, u32 inode, u32 offset, const void *buffer,
                  u32 length, u32 *transferred, u32 *size) {
    struct blockfs_mount *m = mount_at(mount);
    struct blockfs_inode in;
    if (!m || !buffer || !transferred || !size ||
        load_inode(m, inode, &in) || !in.used ||
        in.type != VFS_NODE_REGULAR)
        return -1;
    if (offset > BLOCKFS_FILE_SIZE_MAX || length > BLOCKFS_FILE_SIZE_MAX - offset)
        return -1;
    if (ensure_extent(m, &in)) return -1;
    u32 end = offset + length;
    u32 done = 0;
    while (done < length) {
        u32 pos = offset + done;
        u32 lba = in.start_lba + pos / BLOCK_SECTOR_SIZE;
        u32 skip = pos % BLOCK_SECTOR_SIZE;
        u32 n = BLOCK_SECTOR_SIZE - skip;
        if (n > length - done) n = length - done;
        if (lba >= in.start_lba + in.sectors) return -1;
        u8 sector[BLOCK_SECTOR_SIZE];
        if (block_cache_read(m->device, lba, sector, 1)) return -1;
        copy_bytes(sector + skip, (const u8 *)buffer + done, n);
        if (block_cache_write(m->device, lba, sector, 1)) return -1;
        done += n;
    }
    if (end > in.size) in.size = end;
    if (store_inode(m, inode, &in) || block_cache_flush(m->device))
        return -1;
    *transferred = done;
    *size = in.size;
    return 0;
}

int blockfs_truncate(u32 mount, u32 inode, u32 size, u32 *new_size) {
    struct blockfs_mount *m = mount_at(mount);
    struct blockfs_inode in;
    if (!m || !new_size || size > BLOCKFS_FILE_SIZE_MAX ||
        load_inode(m, inode, &in) || !in.used ||
        in.type != VFS_NODE_REGULAR)
        return -1;
    if (size && ensure_extent(m, &in)) return -1;
    if (in.start_lba && size < in.size) {
        u32 pos = size;
        while (pos < in.size) {
            u32 lba = in.start_lba + pos / BLOCK_SECTOR_SIZE;
            u32 skip = pos % BLOCK_SECTOR_SIZE;
            u32 n = BLOCK_SECTOR_SIZE - skip;
            if (n > in.size - pos) n = in.size - pos;
            u8 sector[BLOCK_SECTOR_SIZE];
            if (block_cache_read(m->device, lba, sector, 1)) return -1;
            for (u32 i = 0; i < n; i++) sector[skip + i] = 0;
            if (block_cache_write(m->device, lba, sector, 1)) return -1;
            pos += n;
        }
    }
    in.size = size;
    if (store_inode(m, inode, &in) || block_cache_flush(m->device))
        return -1;
    *new_size = size;
    return 0;
}
