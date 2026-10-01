#include "adytumfs.h"
#include "adytumfs_format.h"
#include "vfs.h"
#include "block.h"
#include "cache.h"
#include "resource.h"
#include "rtc64.h"

#define ADYTUMFS_MOUNT_MAX 8

// The VFS numbers a mount's files 0..ADYTUMFS_INODE_MAX with 0 as the mount
// root, and uses 0 as the "no backend file" id for ramfs nodes. The on-disk
// format numbers inodes from 1 because a zero dirent target marks a free
// record, so this backend translates through a dense slot table rebuilt by
// walking the tree at attach. The table also carries the name and parent the
// VFS mount scan asks for, which the on-disk inode does not store.
struct adytumfs_slot {
    u64 inode;
    u64 generation;
    u32 parent;
    char name[VFS_NAME_MAX + 1];
};

struct adytumfs_cache {
    struct kernel_object *pages;
    u64 present;
    u64 dirty;
    u32 active;
};

struct adytumfs_mount {
    struct kernel_object *device;
    struct adytumfs_superblock super;
    struct adytumfs_slot slots[ADYTUMFS_INODE_MAX];
    struct adytumfs_cache cache[ADYTUMFS_INODE_MAX];
    u32 active;
};

static struct adytumfs_mount mounts[ADYTUMFS_MOUNT_MAX];

// One staging block for the superblock read and the truncate tail; the
// backend paths are single-CPU, like the rest of the format layer.
static u8 adytumfs_backend_scratch[ADYTUMFS_BLOCK_SIZE];

static struct adytumfs_mount *mount_at(u32 mount) {
    if (!mount || mount >= ADYTUMFS_MOUNT_MAX) return 0;
    struct adytumfs_mount *m = &mounts[mount];
    return m->active ? m : 0;
}

// A slot id is reused once its file is unlinked, while a node that was
// unlinked with an open handle keeps calling into the backend. Every entry
// point that addresses a slot must therefore also see the generation the
// caller captured when the node was scanned or created, and a mismatch is a
// stale handle: fail closed rather than touch the new owner of the slot.
static struct adytumfs_mount *slot_mount(u32 mount, u32 inode,
                                         u64 generation) {
    struct adytumfs_mount *m = mount_at(mount);
    if (!m || inode >= ADYTUMFS_INODE_MAX || !m->slots[inode].inode ||
        m->slots[inode].generation != generation)
        return 0;
    return m;
}

static u32 slot_name_len(const struct adytumfs_slot *slot) {
    u32 length = 0;
    while (length < VFS_NAME_MAX && slot->name[length]) length++;
    return length;
}

// Read the on-disk inode behind a VFS slot id.
static int slot_inode_read(struct adytumfs_mount *m, u32 slot,
                           struct adytumfs_inode *out) {
    if (!m || slot >= ADYTUMFS_INODE_MAX || !m->slots[slot].inode) return -1;
    return adytumfs_inode_read(m->device, &m->super, m->slots[slot].inode,
                               out);
}

// The format stores the VFS node type in the POSIX type bits of the mode.
static u32 slot_type(const struct adytumfs_inode *inode) {
    return (inode->mode & ADYTUMFS_MODE_DIR) ? VFS_NODE_DIRECTORY :
                                               VFS_NODE_REGULAR;
}

// Fill the slot table by walking directories from the root. A record that
// names a dead or out-of-range inode, a record whose type disagrees with the
// target inode, a second name for one inode, or a name past the VFS bound
// means the tree is inconsistent, so the mount fails rather than guesses. The
// queue is bounded by the slot count because every directory owns a slot
// before it is queued, and a parent always owns a lower slot than its
// children, so the table cannot express a cycle.
static int walk_tree(struct adytumfs_mount *m) {
    struct adytumfs_inode root;
    if (adytumfs_inode_read(m->device, &m->super, m->super.root_inode, &root))
        return -1;
    // The mount root is the directory every walk starts from; anything else
    // would have file data parsed as directory records.
    if ((root.mode & ADYTUMFS_MODE_DIR) == 0) return -1;
    m->slots[0].inode = m->super.root_inode;
    m->slots[0].generation = root.generation;
    m->slots[0].parent = 0;
    for (u32 byte = 0; byte <= VFS_NAME_MAX; byte++) m->slots[0].name[byte] = 0;
    u64 queue_inode[ADYTUMFS_INODE_MAX];
    u32 queue_slot[ADYTUMFS_INODE_MAX];
    u32 head = 0;
    u32 tail = 1;
    u32 used = 1;
    queue_inode[0] = m->super.root_inode;
    queue_slot[0] = 0;
    while (head < tail) {
        u64 dir_inode = queue_inode[head];
        u32 dir_slot = queue_slot[head];
        head++;
        u64 cursor = 0;
        for (;;) {
            char name[ADYTUMFS_NAME_MAX];
            u32 name_len = 0;
            u64 target = 0;
            u8 type = 0;
            int step = adytumfs_dir_iter(m->device, &m->super, dir_inode,
                                         &cursor, name, &name_len, &target,
                                         &type);
            if (step == 1) break;
            if (step < 0) return -1;
            struct adytumfs_inode child;
            if (adytumfs_inode_read(m->device, &m->super, target, &child) ||
                child.mode == 0)
                return -1;
            u32 bits = child.mode & (ADYTUMFS_MODE_DIR | ADYTUMFS_MODE_REG);
            if (bits != ADYTUMFS_MODE_DIR && bits != ADYTUMFS_MODE_REG)
                return -1;
            u32 expect = bits == ADYTUMFS_MODE_DIR ? ADYTUMFS_DTYPE_DIR :
                                                     ADYTUMFS_DTYPE_REG;
            if (type != expect) return -1;
            if (!name_len || name_len > VFS_NAME_MAX) return -1;
            for (u32 slot = 0; slot < used; slot++)
                if (m->slots[slot].inode == target) return -1;
            if (used >= ADYTUMFS_INODE_MAX) return -1;
            u32 slot = used++;
            m->slots[slot].inode = target;
            m->slots[slot].generation = child.generation;
            m->slots[slot].parent = dir_slot;
            for (u32 byte = 0; byte <= VFS_NAME_MAX; byte++)
                m->slots[slot].name[byte] = 0;
            for (u32 byte = 0; byte < name_len; byte++)
                m->slots[slot].name[byte] = name[byte];
            if (bits == ADYTUMFS_MODE_DIR) {
                queue_inode[tail] = target;
                queue_slot[tail] = slot;
                tail++;
            }
        }
    }
    return 0;
}

// Resolve a cache slot, dropping it when the page resource it borrows was
// revoked in the meantime.
static struct adytumfs_cache *cache_slot(struct adytumfs_mount *m,
                                         u32 inode) {
    if (!m || inode >= ADYTUMFS_INODE_MAX) return 0;
    struct adytumfs_cache *slot = &m->cache[inode];
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
#define ADYTUMFS_SG_PAGES (BLOCK_IO_SG_SECTORS_MAX / (4096u / BLOCK_SECTOR_SIZE))

// Move count pages between the page resource and the device. lba is the
// device sector of page first; the caller guarantees the pages of the batch
// map to physically adjacent blocks.
static int page_transfer(struct adytumfs_mount *m, struct adytumfs_cache *slot,
                         u32 lba, u32 first, u32 count, u32 op) {
    if (!count || count > ADYTUMFS_SG_PAGES) return -1;
    struct kernel_object *pages[ADYTUMFS_SG_PAGES];
    u32 indices[ADYTUMFS_SG_PAGES];
    u32 offsets[ADYTUMFS_SG_PAGES];
    u32 lengths[ADYTUMFS_SG_PAGES];
    for (u32 index = 0; index < count; index++) {
        pages[index] = slot->pages;
        indices[index] = first + index;
        offsets[index] = 0;
        lengths[index] = 4096;
    }
    struct kernel_object *sg =
        sg_resource_create(pages, indices, offsets, lengths, count);
    if (!sg) return -1;
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

int adytumfs_pages_attach(u32 mount, u32 inode, u64 generation,
                          struct kernel_object *pages) {
    struct adytumfs_mount *m = slot_mount(mount, inode, generation);
    if (!m || !pages || m->cache[inode].active)
        return -1;
    struct adytumfs_inode in;
    if (slot_inode_read(m, inode, &in) || (in.mode & ADYTUMFS_MODE_REG) == 0)
        return -1;
    struct page_resource *resource = page_resource_get(pages);
    if (!resource || resource->pages > ADYTUMFS_FILE_PAGES) return -1;
    struct adytumfs_cache *slot = &m->cache[inode];
    slot->pages = pages;
    slot->present = 0;
    slot->dirty = 0;
    slot->active = 1;
    return 0;
}

int adytumfs_pages_fault(u32 mount, u32 inode, u64 generation, u32 page) {
    struct adytumfs_mount *m = slot_mount(mount, inode, generation);
    struct adytumfs_cache *slot = cache_slot(m, inode);
    struct adytumfs_inode in;
    if (!slot || page >= ADYTUMFS_FILE_PAGES ||
        slot_inode_read(m, inode, &in) || (in.mode & ADYTUMFS_MODE_REG) == 0)
        return -1;
    if (slot->present & (1ull << page)) return 0;
    struct page_resource *resource = page_resource_get(slot->pages);
    if (!resource || page >= resource->pages) return -1;
    u8 *bytes = (u8 *)(uptr_t)resource->physical[page];
    if (!bytes) return -1;
    u64 physical = 0;
    // A page past the file size or past the allocated extents reads as
    // zeroes: the write path grows at sync, not at fault.
    if (page * 4096u >= in.size ||
        adytumfs_inode_map(&in, page, &physical)) {
        for (u32 index = 0; index < 4096; index++) bytes[index] = 0;
        slot->present |= 1ull << page;
        return 0;
    }
    if (page_transfer(m, slot, (u32)(physical * ADYTUMFS_SECTORS_PER_BLOCK),
                      page, 1, BLOCK_OP_READ))
        return -1;
    // The transfer filled the page from the device, so the region must vouch
    // for those bytes exactly like the read path demands.
    if (adytumfs_data_check(m->device, &m->super, physical, bytes)) return -1;
    slot->present |= 1ull << page;
    return 0;
}

int adytumfs_pages_dirty(u32 mount, u32 inode, u64 generation, u32 page,
                         u32 size) {
    struct adytumfs_mount *m = slot_mount(mount, inode, generation);
    struct adytumfs_cache *slot = cache_slot(m, inode);
    struct adytumfs_inode in;
    if (!slot || page >= ADYTUMFS_FILE_PAGES || size > ADYTUMFS_FILE_SIZE_MAX ||
        slot_inode_read(m, inode, &in) || (in.mode & ADYTUMFS_MODE_REG) == 0)
        return -1;
    slot->present |= 1ull << page;
    slot->dirty |= 1ull << page;
    if (size > in.size) {
        in.size = size;
        if (adytumfs_inode_write(m->device, &m->super, m->slots[inode].inode,
                                 &in))
            return -1;
    }
    return 0;
}

int adytumfs_pages_sync(u32 mount, u32 inode, u64 generation) {
    struct adytumfs_mount *m = slot_mount(mount, inode, generation);
    struct adytumfs_cache *slot = cache_slot(m, inode);
    if (!m) return -1;
    if (!slot) return 0;
    struct adytumfs_inode in;
    if (slot_inode_read(m, inode, &in) || (in.mode & ADYTUMFS_MODE_REG) == 0)
        return -1;
    if (!slot->dirty) return 0;
    // Grow to the write-back size, not only over the dirty pages: a write
    // that skipped pages leaves holes whose blocks must exist and read as
    // zeroes. Growth only appends, so attached pages stay valid.
    u64 needed = in.size / ADYTUMFS_BLOCK_SIZE +
                 (in.size % ADYTUMFS_BLOCK_SIZE ? 1 : 0);
    if (needed > ADYTUMFS_FILE_PAGES) needed = ADYTUMFS_FILE_PAGES;
    // Pages backed by blocks that predate this write-back are redirected to
    // fresh copies; pages the grow is about to add are written directly.
    u64 old_blocks = in.blocks;
    if (needed > in.blocks) {
        if (adytumfs_inode_grow(m->device, &m->super, &in, needed)) return -1;
        if (adytumfs_inode_write(m->device, &m->super, m->slots[inode].inode,
                                 &in))
            return -1;
    }
    u32 page = 0;
    while (page < ADYTUMFS_FILE_PAGES) {
        if (!(slot->dirty & (1ull << page))) {
            page++;
            continue;
        }
        u64 physical = 0;
        if (adytumfs_inode_map(&in, page, &physical)) return -1;
        // A redirected page moves alone: its fresh target shares no run with
        // the neighbours, so batching stops at the redirect.
        if (page < old_blocks) {
            u64 stale = 0;
            u64 fresh = 0;
            if (adytumfs_redirect_stage(m->device, &m->super, &in, page,
                                        &stale, &fresh))
                return -1;
            if (page_transfer(m, slot,
                              (u32)(fresh * ADYTUMFS_SECTORS_PER_BLOCK), page,
                              1, BLOCK_OP_WRITE))
                return -1;
            // The page reached the device through a direct transfer, so the
            // new bytes are sealed into the checksum region here rather than
            // through a staging block.
            struct page_resource *resource = page_resource_get(slot->pages);
            u8 *bytes = resource ?
                (u8 *)(uptr_t)resource->physical[page] : 0;
            if (!bytes ||
                adytumfs_data_seal(m->device, &m->super, fresh, bytes))
                return -1;
            if (adytumfs_redirect_commit(m->device, &m->super, &in,
                                         m->slots[inode].inode, page, stale,
                                         fresh))
                return -1;
            slot->dirty &= ~(1ull << page);
            page++;
            continue;
        }
        u32 count = 1;
        // One transfer covers one physically contiguous run, so the batch
        // ends where the next dirty page maps to a non-adjacent block.
        while (count < ADYTUMFS_SG_PAGES &&
               page + count < ADYTUMFS_FILE_PAGES &&
               (slot->dirty & (1ull << (page + count)))) {
            u64 next = 0;
            if (adytumfs_inode_map(&in, page + count, &next) ||
                next != physical + count)
                break;
            count++;
        }
        if (page_transfer(m, slot,
                          (u32)(physical * ADYTUMFS_SECTORS_PER_BLOCK), page,
                          count, BLOCK_OP_WRITE))
            return -1;
        // The pages reached the device through a direct transfer, so the new
        // bytes are sealed into the checksum region here rather than through
        // a staging block.
        struct page_resource *resource = page_resource_get(slot->pages);
        if (!resource) return -1;
        for (u32 index = 0; index < count; index++) {
            u8 *bytes = (u8 *)(uptr_t)resource->physical[page + index];
            if (!bytes ||
                adytumfs_data_seal(m->device, &m->super, physical + index,
                                   bytes))
                return -1;
        }
        for (u32 index = 0; index < count; index++)
            slot->dirty &= ~(1ull << (page + index));
        page += count;
    }
    // Sync is the agreed commit boundary: the redirected pages and the
    // staged metadata land together or not at all.
    return adytumfs_commit(m->device, &m->super);
}

void adytumfs_pages_detach(u32 mount, u32 inode, u64 generation) {
    struct adytumfs_mount *m = slot_mount(mount, inode, generation);
    if (!m) return;
    struct adytumfs_cache *slot = &m->cache[inode];
    slot->pages = 0;
    slot->present = 0;
    slot->dirty = 0;
    slot->active = 0;
}

int adytumfs_format(struct kernel_object *device) {
    // Kept under the VFS-facing name so callers outside the fs layer do not
    // change; the on-disk work lives with the format stack.
    return adytumfs_make(device);
}

int adytumfs_attach(u32 mount, struct kernel_object *device) {
    if (!mount || mount >= ADYTUMFS_MOUNT_MAX || mount_at(mount) || !device)
        return -1;
    // One active mount per device: a second window on the same volume would
    // stage metadata against a superblock copy the first mount is already
    // committing past.
    for (u32 scan = 0; scan < ADYTUMFS_MOUNT_MAX; scan++)
        if (mounts[scan].device == device) return -1;
    // The window is keyed by device pointer, and a released device object
    // can hand that pointer to a new volume: every read below passes
    // through the staging, so the stale window has to go before them.
    adytumfs_window_discard(device);
    struct adytumfs_superblock super;
    if (adytumfs_super_read(device, &super)) return -1;
    // A structurally valid superblock whose tree does not verify is post
    // commit corruption, not a torn commit: the commit order lands data and
    // metadata before the superblock flip, so a torn flip fails the checksum
    // above and only rot gets this far. Falling back to the older generation
    // here would roll the volume back over the damage; the mount fails
    // instead, leaving both generations for an explicit repair pass.
    // v1 defines no ro-compat features; an image asking for one would need a
    // read-only mount, which the VFS contract cannot express.
    if (super.feature_ro_compat) return -1;
    if (adytumfs_verify(device, &super)) return -1;
    if (object_retain(device)) return -1;
    struct adytumfs_mount *m = &mounts[mount];
    for (u32 slot = 0; slot < ADYTUMFS_INODE_MAX; slot++) {
        m->slots[slot].inode = 0;
        m->slots[slot].generation = 0;
        m->slots[slot].parent = 0;
        for (u32 byte = 0; byte <= VFS_NAME_MAX; byte++)
            m->slots[slot].name[byte] = 0;
        m->cache[slot].pages = 0;
        m->cache[slot].present = 0;
        m->cache[slot].dirty = 0;
        m->cache[slot].active = 0;
    }
    m->super = super;
    m->device = device;
    if (walk_tree(m)) {
        m->device = 0;
        object_release(device);
        return -1;
    }
    m->active = 1;
    return 0;
}

void adytumfs_detach(u32 mount) {
    struct adytumfs_mount *m = mount_at(mount);
    if (!m) return;
    if (m->device) {
        for (u32 inode = 0; inode < ADYTUMFS_INODE_MAX; inode++) {
            if (!m->cache[inode].active) continue;
            adytumfs_pages_sync(mount, inode, m->slots[inode].generation);
            adytumfs_pages_detach(mount, inode, m->slots[inode].generation);
        }
        // Unmount is the other commit boundary; pages_sync already committed
        // per file, so this lands windows that only touched raw metadata.
        adytumfs_commit(m->device, &m->super);
        object_release(m->device);
    }
    m->device = 0;
    m->active = 0;
}

u32 adytumfs_inode_count(u32 mount) {
    // The VFS mount scan stacks arrays of this size and attach rejects a
    // fuller image, so the bound is the slot table, not the volume's inode
    // count.
    return mount_at(mount) ? ADYTUMFS_INODE_MAX : 0;
}

int adytumfs_inode_get(u32 mount, u32 inode, u32 *used, u32 *type, u32 *size,
                      u32 *parent, u32 *mode, char *name, u64 *generation) {
    struct adytumfs_mount *m = mount_at(mount);
    if (!m || inode >= ADYTUMFS_INODE_MAX) return -1;
    if (!m->slots[inode].inode) {
        if (used) *used = 0;
        if (type) *type = 0;
        if (size) *size = 0;
        if (parent) *parent = 0;
        if (mode) *mode = 0;
        if (generation) *generation = 0;
        if (name)
            for (u32 byte = 0; byte <= VFS_NAME_MAX; byte++) name[byte] = 0;
        return 0;
    }
    struct adytumfs_inode in;
    if (slot_inode_read(m, inode, &in)) return -1;
    // On-disk sizes are untrusted: clamp to the allocated extents and to the
    // page-resource bound, or a crafted inode drives reads past the mapping.
    u64 bound = in.blocks * ADYTUMFS_BLOCK_SIZE;
    if (bound > ADYTUMFS_FILE_SIZE_MAX) bound = ADYTUMFS_FILE_SIZE_MAX;
    if (used) *used = 1;
    if (type) *type = slot_type(&in);
    if (size) *size = (u32)(in.size < bound ? in.size : bound);
    if (parent) *parent = m->slots[inode].parent;
    if (mode) *mode = in.mode & VFS_MODE_MASK;
    if (generation) *generation = m->slots[inode].generation;
    if (name)
        for (u32 byte = 0; byte <= VFS_NAME_MAX; byte++)
            name[byte] = m->slots[inode].name[byte];
    return 0;
}

int adytumfs_touch(u32 mount, u32 inode, u64 generation, u32 flags) {
    struct adytumfs_mount *m = slot_mount(mount, inode, generation);
    if (!m || !flags) return -1;
    struct adytumfs_inode in;
    if (slot_inode_read(m, inode, &in)) return -1;
    u32 changed = 0;
    u64 now = rtc64_wall_clock();
    // Relatime-lite: a read only refreshes atime while it still trails the
    // last data change, so steady-state reads never dirty the inode.
    if ((flags & ADYTUMFS_TOUCH_ATIME) && in.atime < in.mtime) {
        in.atime = now;
        changed = 1;
    }
    if (flags & ADYTUMFS_TOUCH_MTIME) {
        in.mtime = now;
        changed = 1;
    }
    if (flags & ADYTUMFS_TOUCH_CTIME) {
        in.ctime = now;
        changed = 1;
    }
    if (!changed) return 0;
    return adytumfs_inode_write(m->device, &m->super, m->slots[inode].inode,
                                &in);
}

int adytumfs_inode_meta(u32 mount, u32 inode, u64 generation, u16 *links,
                        u32 *uid, u32 *gid, u64 *atime, u64 *mtime,
                        u64 *ctime) {
    struct adytumfs_mount *m = slot_mount(mount, inode, generation);
    if (!m) return -1;
    struct adytumfs_inode in;
    if (slot_inode_read(m, inode, &in)) return -1;
    if (links) *links = in.links;
    if (uid) *uid = in.uid;
    if (gid) *gid = in.gid;
    if (atime) *atime = in.atime;
    if (mtime) *mtime = in.mtime;
    if (ctime) *ctime = in.ctime;
    return 0;
}

int adytumfs_inode_create(u32 mount, const char *name, u32 parent, u32 type,
                         u32 mode, u32 *inode, u64 *generation) {
    struct adytumfs_mount *m = mount_at(mount);
    if (!m || !name || !name[0] || !inode || parent >= ADYTUMFS_INODE_MAX ||
        (type != VFS_NODE_REGULAR && type != VFS_NODE_DIRECTORY) ||
        (mode & ~VFS_MODE_MASK))
        return -1;
    u32 name_len = 0;
    while (name[name_len] && name_len < VFS_NAME_MAX) name_len++;
    if (name_len == VFS_NAME_MAX && name[name_len]) return -1;
    struct adytumfs_inode dir;
    if (slot_inode_read(m, parent, &dir) || (dir.mode & ADYTUMFS_MODE_DIR) == 0)
        return -1;
    // Reserve the table slot before touching the disk so a full table never
    // leaves an allocated inode behind.
    u32 slot = 0;
    for (u32 index = 1; index < ADYTUMFS_INODE_MAX; index++)
        if (!m->slots[index].inode) {
            slot = index;
            break;
        }
    if (!slot) return -1;
    u16 disk_mode = (u16)(mode | (type == VFS_NODE_DIRECTORY ?
                                      ADYTUMFS_MODE_DIR : ADYTUMFS_MODE_REG));
    u64 target = 0;
    if (adytumfs_create_at(m->device, &m->super, m->slots[parent].inode, name,
                           name_len, disk_mode, &target))
        return -1;
    // The allocator stamps a fresh generation into the on-disk inode, and the
    // slot table must carry the same value the caller will be told to present
    // on every later backend call.
    struct adytumfs_inode fresh;
    if (adytumfs_inode_read(m->device, &m->super, target, &fresh)) return -1;
    m->slots[slot].inode = target;
    m->slots[slot].generation = fresh.generation;
    m->slots[slot].parent = parent;
    for (u32 byte = 0; byte <= VFS_NAME_MAX; byte++)
        m->slots[slot].name[byte] = 0;
    for (u32 byte = 0; byte < name_len; byte++)
        m->slots[slot].name[byte] = name[byte];
    *inode = slot;
    if (generation) *generation = fresh.generation;
    return 0;
}

int adytumfs_inode_remove(u32 mount, u32 inode) {
    struct adytumfs_mount *m = mount_at(mount);
    if (!m || !inode || inode >= ADYTUMFS_INODE_MAX ||
        !m->slots[inode].inode)
        return -1;
    struct adytumfs_inode in;
    if (slot_inode_read(m, inode, &in)) return -1;
    u32 bits = in.mode & (ADYTUMFS_MODE_DIR | ADYTUMFS_MODE_REG);
    if (bits != ADYTUMFS_MODE_DIR && bits != ADYTUMFS_MODE_REG) return -1;
    if (bits == ADYTUMFS_MODE_DIR) {
        // The VFS only unlinks directories it sees as empty; hold the disk to
        // the same rule, because tombstoned records still own a data block.
        char name[ADYTUMFS_NAME_MAX];
        u32 name_len = 0;
        u64 target = 0;
        u8 type = 0;
        u64 cursor = 0;
        if (adytumfs_dir_iter(m->device, &m->super, m->slots[inode].inode,
                              &cursor, name, &name_len, &target, &type) != 1)
            return -1;
    }
    adytumfs_pages_detach(mount, inode, m->slots[inode].generation);
    u32 parent = m->slots[inode].parent;
    u64 real = m->slots[inode].inode;
    // Reclaim the data and persist the emptied inode before the name and the
    // inode slot go: a crash then leaves an emptied file or an orphan, never
    // a live name over blocks a later allocation can reuse.
    if (adytumfs_inode_truncate(m->device, &m->super, &in, 0)) return -1;
    in.size = 0;
    if (adytumfs_inode_write(m->device, &m->super, real, &in)) return -1;
    if (adytumfs_dir_remove(m->device, &m->super, m->slots[parent].inode,
                            m->slots[inode].name,
                            slot_name_len(&m->slots[inode])))
        return -1;
    if (adytumfs_inode_free(m->device, &m->super, real)) return -1;
    m->slots[inode].inode = 0;
    m->slots[inode].parent = 0;
    for (u32 byte = 0; byte <= VFS_NAME_MAX; byte++)
        m->slots[inode].name[byte] = 0;
    return 0;
}

int adytumfs_truncate(u32 mount, u32 inode, u64 generation, u32 size,
                     u32 *new_size) {
    struct adytumfs_mount *m = slot_mount(mount, inode, generation);
    if (!m || !new_size || size > ADYTUMFS_FILE_SIZE_MAX) return -1;
    struct adytumfs_inode in;
    if (slot_inode_read(m, inode, &in) || (in.mode & ADYTUMFS_MODE_REG) == 0)
        return -1;
    u64 blocks = size / ADYTUMFS_BLOCK_SIZE +
                 (size % ADYTUMFS_BLOCK_SIZE ? 1 : 0);
    if (blocks > in.blocks) {
        if (adytumfs_inode_grow(m->device, &m->super, &in, blocks)) return -1;
    } else if (blocks < in.blocks) {
        if (adytumfs_inode_truncate(m->device, &m->super, &in, blocks))
            return -1;
    }
    if (size % ADYTUMFS_BLOCK_SIZE) {
        // Reads clamp to size but a mapping exposes the whole page, so the
        // bytes past the new size inside the kept tail block read as zeroes.
        u64 physical = 0;
        if (adytumfs_inode_map(&in, blocks - 1, &physical)) return -1;
        if (adytumfs_data_read(m->device, &m->super, physical,
                               adytumfs_backend_scratch))
            return -1;
        for (u32 index = size % ADYTUMFS_BLOCK_SIZE;
             index < ADYTUMFS_BLOCK_SIZE; index++)
            adytumfs_backend_scratch[index] = 0;
        if (adytumfs_inode_remap(m->device, &m->super, &in,
                                 m->slots[inode].inode, blocks - 1,
                                 adytumfs_backend_scratch))
            return -1;
    }
    // Truncation changes the data, so mtime and ctime move with the new size.
    u64 now = rtc64_wall_clock();
    in.mtime = now;
    in.ctime = now;
    in.size = size;
    if (adytumfs_inode_write(m->device, &m->super, m->slots[inode].inode,
                             &in) ||
        adytumfs_commit(m->device, &m->super))
        return -1;
    *new_size = size;
    return 0;
}
