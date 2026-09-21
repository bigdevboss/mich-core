#include "blockfs.h"
#include "vfs.h"
#include "block.h"
#include "cache.h"

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

struct blockfs_mount {
    struct kernel_object *device;
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
    if (offset > VFS_FILE_SIZE_MAX || length > VFS_FILE_SIZE_MAX - offset)
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
    if (!m || !new_size || size > VFS_FILE_SIZE_MAX ||
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
