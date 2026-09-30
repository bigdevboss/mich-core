#include "types.h"
#include "object.h"
#include "block.h"
#include "cache.h"
#include "adytumfs.h"
#include "adytumfs_format.h"
#include "vfs.h"
#include "resource.h"

// One staging block for raw device probes and on-disk edits; the test battery
// is single-CPU, so reuse across probes is safe.
static u8 adytumfs_probe_block[ADYTUMFS_BLOCK_SIZE];

static int probe_name_equals(const char *name, const char *expect) {
    u32 index = 0;
    while (expect[index]) {
        if (name[index] != expect[index]) return 0;
        index++;
    }
    return !name[index];
}

// block_io moves one sector per call, so a raw 4 KiB block probe reads in
// eight steps.
static int probe_read_block(struct kernel_object *dev, u64 block, u8 *buffer) {
    for (u32 sector = 0; sector < ADYTUMFS_SECTORS_PER_BLOCK; sector++)
        if (block_io(dev, BLOCK_OP_READ,
                     (u32)(block * ADYTUMFS_SECTORS_PER_BLOCK + sector), 1,
                     buffer + sector * BLOCK_SECTOR_SIZE, BLOCK_SECTOR_SIZE))
            return -1;
    return 0;
}

// Parse the volume superblock through the block cache, which the corruption
// scenarios below also write their edits through.
static int policy_super(struct kernel_object *dev,
                        struct adytumfs_superblock *super) {
    if (adytumfs_block_read(dev, 0, adytumfs_probe_block)) return -1;
    return adytumfs_super_unpack(super, adytumfs_probe_block);
}

// Locate a root directory record by name and leave its data block in the
// staging buffer, so a scenario can edit one field of one record in place.
static int policy_find_record(struct kernel_object *dev,
                              const struct adytumfs_superblock *super,
                              const char *name, u32 name_len, u64 *block,
                              u32 *offset) {
    struct adytumfs_inode dir;
    if (adytumfs_inode_read(dev, super, super->root_inode, &dir)) return -1;
    u64 physical = 0;
    if (adytumfs_inode_map(&dir, 0, &physical)) return -1;
    if (adytumfs_block_read(dev, physical, adytumfs_probe_block)) return -1;
    u32 cursor = 0;
    while (cursor + ADYTUMFS_DIR_HEADER <= ADYTUMFS_BLOCK_SIZE) {
        u64 entry_inode = adytumfs_read_le64(adytumfs_probe_block + cursor);
        u16 rec_len = adytumfs_read_le16(adytumfs_probe_block + cursor + 8);
        u8 stored_len = adytumfs_probe_block[cursor + 10];
        if (rec_len < ADYTUMFS_DIR_HEADER ||
            cursor + rec_len > ADYTUMFS_BLOCK_SIZE)
            return -1;
        if (entry_inode && stored_len == name_len) {
            u32 index = 0;
            while (index < name_len &&
                   adytumfs_probe_block[cursor + 12 + index] == (u8)name[index])
                index++;
            if (index == name_len) {
                *block = physical;
                *offset = cursor;
                return 0;
            }
        }
        cursor += rec_len;
    }
    return -1;
}

// Land a staging-block edit on the device and drop the cached copies, so the
// next mount attempt reads the corruption the way a later boot would.
static int policy_commit(struct kernel_object *dev, u64 block) {
    if (adytumfs_block_write(dev, block, adytumfs_probe_block)) return -1;
    if (block_cache_flush(dev)) return -1;
    block_cache_drop_device((u32)dev->value);
    return 0;
}

int test_adytumfs64(void) {
    u32 objects = object_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    u32 mounts = vfs_mount_active_count();
    u32 devices = block_active_count();
    struct kernel_object *root = vfs_root();
    struct kernel_object *mnt = root ?
        vfs_create(root, "disk", VFS_NODE_DIRECTORY) : 0;
    struct kernel_object *dev = block_create(320, 0);
    u8 payload[16];
    u8 received[16];
    for (u32 i = 0; i < 16; i++) {
        payload[i] = (u8)('A' + i);
        received[i] = 0;
    }
    u32 transferred = 0;
    int valid = root && mnt && dev && !adytumfs_format(dev) &&
        !vfs_mount_adytumfs(mnt, dev);
    struct kernel_object *disk = valid ? vfs_lookup(root, "disk") : 0;
    struct kernel_object *node = disk ?
        vfs_create_mode(disk, "hello", VFS_NODE_REGULAR, 0604) : 0;
    struct kernel_object *folder = disk ?
        vfs_create_mode(disk, "folder", VFS_NODE_DIRECTORY, 0711) : 0;
    struct kernel_object *nested = folder ?
        vfs_create_mode(folder, "nested", VFS_NODE_REGULAR, 0620) : 0;
    struct kernel_object *opened = node ? vfs_open(node) : 0;
    struct vfs_node_info info;
    valid = valid && disk && node && folder && nested && opened &&
        !vfs_write(opened, 0, payload, sizeof(payload), &transferred) &&
        transferred == sizeof(payload) &&
        !vfs_read(opened, 0, received, sizeof(received), &transferred) &&
        transferred == sizeof(received) &&
        !vfs_stat(opened, &info) &&
        info.filesystem == VFS_FILESYSTEM_ADYTUMFS &&
        info.size == sizeof(payload) && info.mode == 0604 && !info.readonly;
    for (u32 i = 0; i < 16; i++)
        if (received[i] != payload[i]) valid = 0;
    valid = valid && !vfs_truncate(opened, 4) &&
        !vfs_stat(node, &info) && info.size == 4 &&
        !vfs_read(opened, 0, received, sizeof(received), &transferred) &&
        transferred == 4 && vfs_unmount(mnt) < 0;
    if (opened) {
        object_release(opened);
        opened = 0;
    }

    int first_unmount = mnt && !vfs_unmount(mnt);
    int remounted = first_unmount && !vfs_mount_adytumfs(mnt, dev);
    struct kernel_object *fresh_directory = remounted ?
        vfs_lookup(root, "disk") : 0;
    struct kernel_object *fresh = fresh_directory ?
        vfs_lookup(fresh_directory, "hello") : 0;
    struct kernel_object *fresh_folder = fresh_directory ?
        vfs_lookup(fresh_directory, "folder") : 0;
    struct kernel_object *fresh_nested = fresh_folder ?
        vfs_lookup(fresh_folder, "nested") : 0;
    struct kernel_object *reopened = fresh ? vfs_open(fresh) : 0;
    valid = valid && first_unmount && remounted && fresh_directory && fresh &&
        fresh_folder && fresh_nested && !vfs_stat(fresh, &info) &&
        info.mode == 0604 && info.size == 4 &&
        !vfs_stat(fresh_folder, &info) && info.mode == 0711 &&
        !vfs_stat(fresh_nested, &info) && info.mode == 0620 && reopened &&
        vfs_unmount(mnt) < 0;

    // Ask the backend for the slot picture the way the mount scan does: the
    // slot table carries the name and parent the walk rebuilt, the inode
    // carries the type, mode, and the truncated size that reached the disk.
    u32 mount_id = 0;
    for (u32 probe = 1; probe < VFS_MOUNT_MAX; probe++)
        if (adytumfs_inode_count(probe)) {
            mount_id = probe;
            break;
        }
    u32 used = 0, type = 0, size = 0, parent = 0, mode = 0;
    char name[VFS_NAME_MAX];
    valid = valid && mount_id &&
        !adytumfs_inode_get(mount_id, 1, &used, &type, &size, &parent, &mode,
                           name, 0) &&
        used == 1 && type == VFS_NODE_REGULAR && size == 4 && parent == 0 &&
        mode == 0604 && probe_name_equals(name, "hello") &&
        !adytumfs_inode_get(mount_id, 2, &used, &type, &size, &parent, &mode,
                           name, 0) &&
        used == 1 && type == VFS_NODE_DIRECTORY && parent == 0 &&
        mode == 0711 && probe_name_equals(name, "folder") &&
        !adytumfs_inode_get(mount_id, 3, &used, &type, &size, &parent, &mode,
                           name, 0) &&
        used == 1 && type == VFS_NODE_REGULAR && parent == 2 &&
        mode == 0620 && probe_name_equals(name, "nested");
    if (reopened) {
        object_release(reopened);
        reopened = 0;
    }
    int second_unmount = fresh_directory && fresh_folder &&
        !vfs_unlink(fresh_directory, "hello") &&
        !vfs_unlink(fresh_folder, "nested") &&
        !vfs_unlink(fresh_directory, "folder") && !vfs_unmount(mnt);
    int final_remount = second_unmount && !vfs_mount_adytumfs(mnt, dev);
    struct kernel_object *final_directory = final_remount ?
        vfs_lookup(root, "disk") : 0;
    struct kernel_object *missing = final_directory ?
        vfs_lookup(final_directory, "hello") : 0;
    struct kernel_object *stale = final_remount && node ? vfs_open(node) : 0;
    valid = valid && second_unmount && final_remount &&
        vfs_stat(node, &info) < 0 && !stale && !missing;
    if (stale) object_release(stale);
    if (missing) object_release(missing);
    if (final_directory) object_release(final_directory);
    if (final_remount && vfs_unmount(mnt)) valid = 0;
    if (fresh_nested) object_release(fresh_nested);
    if (fresh_folder) object_release(fresh_folder);
    if (fresh) object_release(fresh);
    if (fresh_directory) object_release(fresh_directory);
    if (nested) object_release(nested);
    if (folder) object_release(folder);
    if (node) object_release(node);
    if (disk) object_release(disk);
    if (root) vfs_unlink(root, "disk");
    if (mnt) object_release(mnt);
    if (root) object_release(root);
    if (dev) object_release(dev);
    valid = valid && object_active_count() == objects &&
        vfs_node_active_count() == nodes &&
        vfs_file_active_count() == files &&
        vfs_mount_active_count() == mounts &&
        block_active_count() == devices;
    return valid ? 0 : -1;
}

int test_adytumfs_pages64(void) {
    u32 objects = object_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    u32 mounts = vfs_mount_active_count();
    u32 devices = block_active_count();
    struct kernel_object *root = vfs_root();
    struct kernel_object *mnt = root ?
        vfs_create(root, "pages", VFS_NODE_DIRECTORY) : 0;
    struct kernel_object *dev = block_create(320, 0);
    int valid = root && mnt && dev && !adytumfs_format(dev) &&
        !vfs_mount_adytumfs(mnt, dev);
    struct kernel_object *disk = valid ? vfs_lookup(root, "pages") : 0;
    struct kernel_object *node = disk ?
        vfs_create(disk, "wide", VFS_NODE_REGULAR) : 0;
    struct kernel_object *opened = node ? vfs_open(node) : 0;
    valid = valid && disk && node && opened;

    // Two full pages plus a tail, so the flush has to walk more than one
    // dirty bit and the partial page keeps its untouched bytes.
    u32 span = 3 * 4096;
    u32 transferred = 0;
    for (u32 offset = 0; offset < span && valid; offset += 512) {
        u8 chunk[512];
        for (u32 index = 0; index < sizeof(chunk); index++)
            chunk[index] = (u8)(offset / 512 + index);
        if (vfs_write(opened, offset, chunk, sizeof(chunk), &transferred) ||
            transferred != sizeof(chunk))
            valid = 0;
    }

    // Write-back: nothing reached the device yet. Parse the superblock off
    // the raw device, resolve "wide" through the format layer, and read its
    // inode slot raw: it must still be the formatted free slot.
    u32 size = 0;
    struct vfs_node_info info;
    struct adytumfs_superblock probe;
    u64 wide_inode = 0;
    valid = valid && !vfs_stat(opened, &info) && info.size == span &&
        !probe_read_block(dev, 0, adytumfs_probe_block) &&
        !adytumfs_super_unpack(&probe, adytumfs_probe_block) &&
        probe.format_version == ADYTUMFS_FORMAT_VERSION &&
        probe.total_blocks == 320u / ADYTUMFS_SECTORS_PER_BLOCK &&
        probe.root_inode == ADYTUMFS_ROOT_INODE &&
        !adytumfs_dir_lookup(dev, &probe, probe.root_inode, "wide", 4,
                             &wide_inode) &&
        wide_inode;
    // Inode numbers index the table directly and slot 0 stays unused, so
    // inode 1 (the root) is the first byte offset that exists.
    u64 table_block = probe.inode_table_start +
        wide_inode / ADYTUMFS_INODES_PER_BLOCK;
    u32 table_slot = (u32)((wide_inode % ADYTUMFS_INODES_PER_BLOCK) *
                           ADYTUMFS_INODE_SIZE);
    struct adytumfs_inode probe_inode;
    valid = valid &&
        !probe_read_block(dev, table_block, adytumfs_probe_block) &&
        !adytumfs_inode_unpack(&probe_inode,
                               adytumfs_probe_block + table_slot) &&
        probe_inode.mode == 0;

    valid = valid && !vfs_sync(opened);

    // The flush landed: the raw slot now describes the grown file, and the
    // extent blocks on the device hold the written pattern.
    valid = valid &&
        !probe_read_block(dev, table_block, adytumfs_probe_block) &&
        !adytumfs_inode_unpack(&probe_inode,
                               adytumfs_probe_block + table_slot) &&
        (probe_inode.mode & ADYTUMFS_MODE_REG) &&
        probe_inode.size == span &&
        probe_inode.blocks == span / ADYTUMFS_BLOCK_SIZE;
    for (u64 logical = 0; logical < span / ADYTUMFS_BLOCK_SIZE && valid;
         logical++) {
        u64 physical = 0;
        valid = !adytumfs_inode_map(&probe_inode, logical, &physical) &&
            !probe_read_block(dev, physical, adytumfs_probe_block);
        for (u32 sector = 0;
             sector < ADYTUMFS_SECTORS_PER_BLOCK && valid; sector++)
            for (u32 index = 0; index < BLOCK_SECTOR_SIZE && valid; index++)
                if (adytumfs_probe_block[sector * BLOCK_SECTOR_SIZE + index] !=
                    (u8)(logical * ADYTUMFS_SECTORS_PER_BLOCK + sector + index))
                    valid = 0;
    }

    // Read it all back through a fresh cache after the remount below.
    u8 got[512];
    for (u32 offset = 0; offset < span && valid; offset += 512) {
        if (vfs_read(opened, offset, got, sizeof(got), &transferred) ||
            transferred != sizeof(got))
            valid = 0;
        for (u32 index = 0; index < sizeof(got) && valid; index++)
            if (got[index] != (u8)(offset / 512 + index)) valid = 0;
    }

    struct kernel_object *pages = opened ? vfs_file_pages(opened, &size) : 0;
    struct page_resource *resource = pages ? page_resource_get(pages) : 0;
    valid = valid && pages && resource && resource->pages >= span / 4096 &&
        size == span;
    if (pages) object_release(pages);

    if (opened) {
        object_release(opened);
        opened = 0;
    }
    if (node) {
        object_release(node);
        node = 0;
    }
    if (disk) {
        object_release(disk);
        disk = 0;
    }

    // Survives a remount: the flush really landed on the device.
    if (mnt && vfs_unmount(mnt)) valid = 0;
    if (mnt && vfs_mount_adytumfs(mnt, dev)) valid = 0;
    struct kernel_object *again = valid ? vfs_lookup(root, "pages") : 0;
    struct kernel_object *reopened_node = again ?
        vfs_lookup(again, "wide") : 0;
    struct kernel_object *reopened = reopened_node ?
        vfs_open(reopened_node) : 0;
    valid = valid && again && reopened_node && reopened;
    for (u32 offset = 0; offset < span && valid; offset += 512) {
        if (vfs_read(reopened, offset, got, sizeof(got), &transferred) ||
            transferred != sizeof(got))
            valid = 0;
        for (u32 index = 0; index < sizeof(got) && valid; index++)
            if (got[index] != (u8)(offset / 512 + index)) valid = 0;
    }

    if (reopened) object_release(reopened);
    if (reopened_node) object_release(reopened_node);
    if (again) object_release(again);
    if (mnt && vfs_unmount(mnt)) valid = 0;
    if (root) vfs_unlink(root, "pages");
    if (mnt) object_release(mnt);
    if (root) object_release(root);
    if (dev) object_release(dev);
    valid = valid && object_active_count() == objects &&
        vfs_node_active_count() == nodes &&
        vfs_file_active_count() == files &&
        vfs_mount_active_count() == mounts &&
        block_active_count() == devices;
    return valid ? 0 : -1;
}

int test_adytumfs_policy64(void) {
    u32 objects = object_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    u32 mounts = vfs_mount_active_count();
    u32 devices = block_active_count();
    struct kernel_object *root = vfs_root();
    int valid = root != 0;

    // A dirent naming an inode the table no longer backs must fail the mount
    // rather than surface a file with another inode's content.
    {
        struct kernel_object *dev = block_create(320, 0);
        struct kernel_object *point = root ?
            vfs_create(root, "policy", VFS_NODE_DIRECTORY) : 0;
        int ok = dev && point && !adytumfs_format(dev) &&
            !vfs_mount_adytumfs(point, dev);
        struct kernel_object *disk = ok ? vfs_lookup(root, "policy") : 0;
        struct kernel_object *node = disk ?
            vfs_create(disk, "hello", VFS_NODE_REGULAR) : 0;
        ok = ok && disk && node;
        if (node) object_release(node);
        if (disk) object_release(disk);
        ok = ok && !vfs_unmount(point);
        struct adytumfs_superblock super;
        u64 target = 0;
        ok = ok && !policy_super(dev, &super) &&
            !adytumfs_dir_lookup(dev, &super, super.root_inode, "hello", 5,
                                 &target) &&
            target;
        u64 table = super.inode_table_start +
            target / ADYTUMFS_INODES_PER_BLOCK;
        u32 slot = (u32)((target % ADYTUMFS_INODES_PER_BLOCK) *
                         ADYTUMFS_INODE_SIZE);
        ok = ok && !adytumfs_block_read(dev, table, adytumfs_probe_block);
        if (ok)
            for (u32 index = 0; index < ADYTUMFS_INODE_SIZE; index++)
                adytumfs_probe_block[slot + index] = 0;
        ok = ok && !policy_commit(dev, table) &&
            vfs_mount_adytumfs(point, dev) < 0;
        valid = valid && ok;
        if (point) object_release(point);
        if (root) vfs_unlink(root, "policy");
        if (dev) object_release(dev);
    }

    // A dirent whose type disagrees with the target inode means the tree
    // cannot be trusted, so the mount must fail.
    {
        struct kernel_object *dev = block_create(320, 0);
        struct kernel_object *point = root ?
            vfs_create(root, "policy", VFS_NODE_DIRECTORY) : 0;
        int ok = dev && point && !adytumfs_format(dev) &&
            !vfs_mount_adytumfs(point, dev);
        struct kernel_object *disk = ok ? vfs_lookup(root, "policy") : 0;
        struct kernel_object *node = disk ?
            vfs_create(disk, "hello", VFS_NODE_REGULAR) : 0;
        ok = ok && disk && node;
        if (node) object_release(node);
        if (disk) object_release(disk);
        ok = ok && !vfs_unmount(point);
        struct adytumfs_superblock super;
        u64 block = 0;
        u32 at = 0;
        ok = ok && !policy_super(dev, &super) &&
            !policy_find_record(dev, &super, "hello", 5, &block, &at);
        if (ok) adytumfs_probe_block[at + 11] = ADYTUMFS_DTYPE_DIR;
        ok = ok && !policy_commit(dev, block) &&
            vfs_mount_adytumfs(point, dev) < 0;
        valid = valid && ok;
        if (point) object_release(point);
        if (root) vfs_unlink(root, "policy");
        if (dev) object_release(dev);
    }

    // Two names for one inode would alias one file under two vnodes, so the
    // mount must fail.
    {
        struct kernel_object *dev = block_create(320, 0);
        struct kernel_object *point = root ?
            vfs_create(root, "policy", VFS_NODE_DIRECTORY) : 0;
        int ok = dev && point && !adytumfs_format(dev) &&
            !vfs_mount_adytumfs(point, dev);
        struct kernel_object *disk = ok ? vfs_lookup(root, "policy") : 0;
        struct kernel_object *first = disk ?
            vfs_create(disk, "a", VFS_NODE_REGULAR) : 0;
        struct kernel_object *second = disk ?
            vfs_create(disk, "b", VFS_NODE_REGULAR) : 0;
        ok = ok && disk && first && second;
        if (first) object_release(first);
        if (second) object_release(second);
        if (disk) object_release(disk);
        ok = ok && !vfs_unmount(point);
        struct adytumfs_superblock super;
        u64 alias = 0;
        u64 block = 0;
        u32 at = 0;
        ok = ok && !policy_super(dev, &super) &&
            !adytumfs_dir_lookup(dev, &super, super.root_inode, "a", 1,
                                 &alias) &&
            alias &&
            !policy_find_record(dev, &super, "b", 1, &block, &at);
        if (ok) adytumfs_write_le64(adytumfs_probe_block + at, alias);
        ok = ok && !policy_commit(dev, block) &&
            vfs_mount_adytumfs(point, dev) < 0;
        valid = valid && ok;
        if (point) object_release(point);
        if (root) vfs_unlink(root, "policy");
        if (dev) object_release(dev);
    }

    // An inode no dirent names is invisible, not fatal: the mount succeeds,
    // the orphan stays unreached, and the table still admits new files.
    {
        struct kernel_object *dev = block_create(320, 0);
        struct kernel_object *point = root ?
            vfs_create(root, "policy", VFS_NODE_DIRECTORY) : 0;
        int ok = dev && point && !adytumfs_format(dev) &&
            !vfs_mount_adytumfs(point, dev);
        struct kernel_object *disk = ok ? vfs_lookup(root, "policy") : 0;
        struct kernel_object *first = disk ?
            vfs_create(disk, "a", VFS_NODE_REGULAR) : 0;
        struct kernel_object *second = disk ?
            vfs_create(disk, "b", VFS_NODE_REGULAR) : 0;
        ok = ok && disk && first && second;
        if (first) object_release(first);
        if (second) object_release(second);
        if (disk) object_release(disk);
        ok = ok && !vfs_unmount(point);
        struct adytumfs_superblock super;
        u64 block = 0;
        u32 at = 0;
        ok = ok && !policy_super(dev, &super) &&
            !policy_find_record(dev, &super, "b", 1, &block, &at);
        if (ok) adytumfs_write_le64(adytumfs_probe_block + at, 0);
        ok = ok && !policy_commit(dev, block) &&
            !vfs_mount_adytumfs(point, dev);
        struct kernel_object *fresh = ok ? vfs_lookup(root, "policy") : 0;
        struct kernel_object *kept = fresh ? vfs_lookup(fresh, "a") : 0;
        struct kernel_object *gone = fresh ? vfs_lookup(fresh, "b") : 0;
        struct kernel_object *added = fresh ?
            vfs_create(fresh, "c", VFS_NODE_REGULAR) : 0;
        ok = ok && fresh && kept && added && !gone;
        if (added) object_release(added);
        if (gone) object_release(gone);
        if (kept) object_release(kept);
        if (fresh) object_release(fresh);
        ok = ok && point && !vfs_unmount(point);
        valid = valid && ok;
        if (point) object_release(point);
        if (root) vfs_unlink(root, "policy");
        if (dev) object_release(dev);
    }

    // More reachable inodes than the mount's slot bound must fail the mount,
    // so the volume cannot silently drop files. Slot 0 of the table is
    // unused, so a 512-sector volume carries 32 on-disk inodes, double the
    // VFS bound, and the 17th reachable inode is placed by the format layer
    // itself.
    {
        struct kernel_object *dev = block_create(512, 0);
        struct kernel_object *point = root ?
            vfs_create(root, "policy", VFS_NODE_DIRECTORY) : 0;
        int ok = dev && point && !adytumfs_format(dev) &&
            !vfs_mount_adytumfs(point, dev);
        struct kernel_object *disk = ok ? vfs_lookup(root, "policy") : 0;
        for (u32 index = 0; index < 15 && ok; index++) {
            char label[4];
            label[0] = 'f';
            label[1] = (char)('0' + index / 10);
            label[2] = (char)('0' + index % 10);
            label[3] = 0;
            struct kernel_object *node = disk ?
                vfs_create(disk, label, VFS_NODE_REGULAR) : 0;
            ok = node != 0;
            if (node) object_release(node);
        }
        ok = ok && disk;
        if (disk) object_release(disk);
        ok = ok && point && !vfs_unmount(point);
        struct adytumfs_superblock super;
        u64 extra = 0;
        ok = ok && !policy_super(dev, &super) &&
            !adytumfs_inode_alloc(dev, &super,
                                  (u16)(ADYTUMFS_MODE_REG | 0644u), &extra) &&
            extra &&
            !adytumfs_dir_add(dev, &super, super.root_inode, "extra", 5,
                              extra, ADYTUMFS_DTYPE_REG) &&
            !block_cache_flush(dev);
        if (dev) block_cache_drop_device((u32)dev->value);
        ok = ok && vfs_mount_adytumfs(point, dev) < 0;
        valid = valid && ok;
        if (point) object_release(point);
        if (root) vfs_unlink(root, "policy");
        if (dev) object_release(dev);
    }

    // A name longer than the VFS bound is legal on disk but cannot be
    // surfaced, so the mount must fail rather than truncate the name.
    {
        struct kernel_object *dev = block_create(320, 0);
        struct kernel_object *point = root ?
            vfs_create(root, "policy", VFS_NODE_DIRECTORY) : 0;
        int ok = dev && point && !adytumfs_format(dev) &&
            !vfs_mount_adytumfs(point, dev);
        struct kernel_object *disk = ok ? vfs_lookup(root, "policy") : 0;
        struct kernel_object *node = disk ?
            vfs_create(disk, "hello", VFS_NODE_REGULAR) : 0;
        ok = ok && disk && node;
        if (node) object_release(node);
        if (disk) object_release(disk);
        ok = ok && !vfs_unmount(point);
        static const char long_name[] =
            "abcdefghijklmnopqrstuvwxyz0123456789xyz";
        struct adytumfs_superblock super;
        u64 target = 0;
        ok = ok && !policy_super(dev, &super) &&
            !adytumfs_dir_lookup(dev, &super, super.root_inode, "hello", 5,
                                 &target) &&
            target &&
            !adytumfs_dir_add(dev, &super, super.root_inode, long_name,
                              sizeof(long_name) - 1, target,
                              ADYTUMFS_DTYPE_REG) &&
            !block_cache_flush(dev);
        if (dev) block_cache_drop_device((u32)dev->value);
        ok = ok && vfs_mount_adytumfs(point, dev) < 0;
        valid = valid && ok;
        if (point) object_release(point);
        if (root) vfs_unlink(root, "policy");
        if (dev) object_release(dev);
    }

    if (root) object_release(root);
    valid = valid && object_active_count() == objects &&
        vfs_node_active_count() == nodes &&
        vfs_file_active_count() == files &&
        vfs_mount_active_count() == mounts &&
        block_active_count() == devices;
    return valid ? 0 : -1;
}

// Payloads are short byte ramps so a leaked or overwritten page is caught by
// one comparison instead of a full buffer audit.
static void reuse_fill(u8 *buffer, u32 length, u8 base) {
    for (u32 index = 0; index < length; index++)
        buffer[index] = (u8)(base + index);
}

static int reuse_matches(const u8 *buffer, u32 length, u8 base) {
    for (u32 index = 0; index < length; index++)
        if (buffer[index] != (u8)(base + index)) return 0;
    return 1;
}

int test_adytumfs_reuse64(void) {
    u32 objects = object_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    u32 mounts = vfs_mount_active_count();
    u32 devices = block_active_count();
    struct kernel_object *root = vfs_root();
    int valid = root != 0;

    // A handle that outlives its unlink keeps its vnode, and the freed slot
    // can be reused by the next create. Every later backend call through the
    // stale handle must fail closed instead of reaching the new owner.
    {
        struct kernel_object *dev = block_create(320, 0);
        struct kernel_object *point = root ?
            vfs_create(root, "reuse", VFS_NODE_DIRECTORY) : 0;
        int ok = dev && point && !adytumfs_format(dev) &&
            !vfs_mount_adytumfs(point, dev);
        struct kernel_object *disk = ok ? vfs_lookup(root, "reuse") : 0;
        u32 mount_id = 0;
        for (u32 probe = 1; probe < VFS_MOUNT_MAX; probe++)
            if (adytumfs_inode_count(probe)) {
                mount_id = probe;
                break;
            }
        u32 used = 0, type = 0, size = 0, parent = 0, mode = 0, moved = 0;
        char name[VFS_NAME_MAX];
        u64 first_generation = 0;
        u64 second_generation = 0;
        u64 third_generation = 0;
        u64 fourth_generation = 0;
        u8 payload[16];
        u8 received[16];

        // Slot 1: a stale handle with a live page cache must fail its reads
        // and writes once another file owns the slot.
        struct kernel_object *first = disk ?
            vfs_create(disk, "first", VFS_NODE_REGULAR) : 0;
        struct kernel_object *first_file = first ? vfs_open(first) : 0;
        reuse_fill(payload, sizeof(payload), 0x10);
        ok = ok && disk && mount_id && first && first_file &&
            !adytumfs_inode_get(mount_id, 1, &used, &type, &size, &parent,
                               &mode, name, &first_generation) &&
            used == 1 && probe_name_equals(name, "first") &&
            first_generation &&
            !vfs_write(first_file, 0, payload, sizeof(payload), &moved) &&
            moved == sizeof(payload) &&
            !vfs_unlink(disk, "first");
        struct kernel_object *second = disk ?
            vfs_create(disk, "second", VFS_NODE_REGULAR) : 0;
        struct kernel_object *second_file = second ? vfs_open(second) : 0;
        reuse_fill(payload, sizeof(payload), 0x20);
        ok = ok && second && second_file &&
            !adytumfs_inode_get(mount_id, 1, &used, &type, &size, &parent,
                               &mode, name, &second_generation) &&
            used == 1 && probe_name_equals(name, "second") &&
            second_generation && second_generation != first_generation &&
            !vfs_write(second_file, 0, payload, sizeof(payload), &moved) &&
            moved == sizeof(payload) &&
            !vfs_sync(second_file) &&
            vfs_read(first_file, 0, received, sizeof(received), &moved) < 0 &&
            vfs_write(first_file, 0, payload, sizeof(payload), &moved) < 0 &&
            !vfs_read(second_file, 0, received, sizeof(received), &moved) &&
            moved == sizeof(received) &&
            reuse_matches(received, sizeof(received), 0x20);

        // Slot 1 teardown: releasing the stale vnode must not detach the new
        // owner's cache, or its dirty pages would never reach the disk.
        reuse_fill(payload, sizeof(payload), 0x30);
        ok = ok && !vfs_write(second_file, 4096, payload, sizeof(payload),
                              &moved) &&
            moved == sizeof(payload);
        if (first_file) {
            object_release(first_file);
            first_file = 0;
        }
        if (first) {
            object_release(first);
            first = 0;
        }
        ok = ok && !vfs_sync(second_file) &&
            !vfs_read(second_file, 4096, received, sizeof(received), &moved) &&
            moved == sizeof(received) &&
            reuse_matches(received, sizeof(received), 0x30);

        // Slot 2: a handle that never touched the disk before its unlink is
        // the sharpest form of the hazard, because its first write would
        // attach a page cache over the reused slot's new inode.
        struct kernel_object *third = disk ?
            vfs_create(disk, "third", VFS_NODE_REGULAR) : 0;
        struct kernel_object *third_file = third ? vfs_open(third) : 0;
        ok = ok && third && third_file &&
            !adytumfs_inode_get(mount_id, 2, &used, &type, &size, &parent,
                               &mode, name, &third_generation) &&
            used == 1 && third_generation &&
            !vfs_unlink(disk, "third");
        struct kernel_object *fourth = disk ?
            vfs_create(disk, "fourth", VFS_NODE_REGULAR) : 0;
        struct kernel_object *fourth_file = fourth ? vfs_open(fourth) : 0;
        reuse_fill(payload, sizeof(payload), 0x40);
        ok = ok && fourth && fourth_file &&
            !adytumfs_inode_get(mount_id, 2, &used, &type, &size, &parent,
                               &mode, name, &fourth_generation) &&
            used == 1 && fourth_generation &&
            fourth_generation != third_generation &&
            vfs_write(third_file, 0, payload, sizeof(payload), &moved) < 0;
        reuse_fill(payload, sizeof(payload), 0x50);
        ok = ok && !vfs_write(fourth_file, 0, payload, sizeof(payload),
                              &moved) &&
            moved == sizeof(payload) &&
            !vfs_sync(fourth_file) &&
            !vfs_read(fourth_file, 0, received, sizeof(received), &moved) &&
            moved == sizeof(received) &&
            reuse_matches(received, sizeof(received), 0x50);

        // The stale slot 2 handle must also drop without touching the new
        // owner, and both files must come back whole after a remount.
        if (third_file) {
            object_release(third_file);
            third_file = 0;
        }
        if (third) {
            object_release(third);
            third = 0;
        }
        ok = ok && !vfs_sync(fourth_file);
        if (fourth_file) object_release(fourth_file);
        if (fourth) object_release(fourth);
        if (second_file) object_release(second_file);
        if (second) object_release(second);
        if (disk) object_release(disk);
        ok = ok && !vfs_unmount(point) && !vfs_mount_adytumfs(point, dev);
        struct kernel_object *fresh = ok ? vfs_lookup(root, "reuse") : 0;
        struct kernel_object *kept_second = fresh ?
            vfs_lookup(fresh, "second") : 0;
        struct kernel_object *kept_fourth = fresh ?
            vfs_lookup(fresh, "fourth") : 0;
        struct kernel_object *second_read = kept_second ?
            vfs_open(kept_second) : 0;
        struct kernel_object *fourth_read = kept_fourth ?
            vfs_open(kept_fourth) : 0;
        ok = ok && fresh && kept_second && kept_fourth && second_read &&
            fourth_read && !vfs_lookup(fresh, "first") &&
            !vfs_lookup(fresh, "third") &&
            !vfs_read(second_read, 0, received, sizeof(received), &moved) &&
            moved == sizeof(received) &&
            reuse_matches(received, sizeof(received), 0x20) &&
            !vfs_read(second_read, 4096, received, sizeof(received), &moved) &&
            moved == sizeof(received) &&
            reuse_matches(received, sizeof(received), 0x30) &&
            !vfs_read(fourth_read, 0, received, sizeof(received), &moved) &&
            moved == sizeof(received) &&
            reuse_matches(received, sizeof(received), 0x50);
        if (second_read) object_release(second_read);
        if (fourth_read) object_release(fourth_read);
        if (kept_second) object_release(kept_second);
        if (kept_fourth) object_release(kept_fourth);
        if (fresh) object_release(fresh);
        ok = ok && !vfs_unmount(point);
        valid = valid && ok;
        if (point) object_release(point);
        if (root) vfs_unlink(root, "reuse");
        if (dev) object_release(dev);
    }

    if (root) object_release(root);
    valid = valid && object_active_count() == objects &&
        vfs_node_active_count() == nodes &&
        vfs_file_active_count() == files &&
        vfs_mount_active_count() == mounts &&
        block_active_count() == devices;
    return valid ? 0 : -1;
}

// Stage the baseline every integrity scenario corrupts: one volume, two
// one-block files, written and synced through the VFS, then unmounted, so the
// on-disk state is exactly what a clean session leaves behind. Returns 0 with
// the device, the mount point, the parsed superblock, and both file inodes.
static int integrity_stage(struct kernel_object *root,
                           struct kernel_object **dev_out,
                           struct kernel_object **point_out,
                           struct adytumfs_superblock *super,
                           u64 *data_inode, u64 *other_inode) {
    struct kernel_object *dev = block_create(320, 0);
    struct kernel_object *point = root ?
        vfs_create(root, "integrity", VFS_NODE_DIRECTORY) : 0;
    int ok = dev && point && !adytumfs_format(dev) &&
        !vfs_mount_adytumfs(point, dev);
    struct kernel_object *disk = ok ? vfs_lookup(root, "integrity") : 0;
    struct kernel_object *data = disk ?
        vfs_create_mode(disk, "data", VFS_NODE_REGULAR, 0600) : 0;
    struct kernel_object *other = disk ?
        vfs_create_mode(disk, "other", VFS_NODE_REGULAR, 0600) : 0;
    struct kernel_object *data_file = data ? vfs_open(data) : 0;
    struct kernel_object *other_file = other ? vfs_open(other) : 0;
    u32 moved = 0;
    u8 payload[16];
    reuse_fill(payload, sizeof(payload), 0x60);
    ok = ok && data && other && data_file && other_file &&
        !vfs_write(data_file, 0, payload, sizeof(payload), &moved) &&
        moved == sizeof(payload) && !vfs_sync(data_file) &&
        !vfs_write(other_file, 0, payload, sizeof(payload), &moved) &&
        moved == sizeof(payload) && !vfs_sync(other_file);
    if (data_file) object_release(data_file);
    if (other_file) object_release(other_file);
    if (data) object_release(data);
    if (other) object_release(other);
    if (disk) object_release(disk);
    ok = ok && point && !vfs_unmount(point) && !policy_super(dev, super) &&
        !adytumfs_dir_lookup(dev, super, super->root_inode, "data", 4,
                             data_inode) && *data_inode &&
        !adytumfs_dir_lookup(dev, super, super->root_inode, "other", 5,
                             other_inode) && *other_inode;
    if (!ok) {
        if (point) object_release(point);
        if (dev) object_release(dev);
        return -1;
    }
    *dev_out = dev;
    *point_out = point;
    return 0;
}

// Land the staged edits and drop the cached copies, so the next mount reads
// the bytes the way a fresh boot would.
static int integrity_commit(struct kernel_object *dev) {
    if (block_cache_flush(dev)) return -1;
    block_cache_drop_device((u32)dev->value);
    return 0;
}

static void integrity_cleanup(struct kernel_object *root,
                              struct kernel_object *point,
                              struct kernel_object *dev) {
    if (point) object_release(point);
    if (root) vfs_unlink(root, "integrity");
    if (dev) object_release(dev);
}

int test_adytumfs_integrity64(void) {
    u32 objects = object_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    u32 mounts = vfs_mount_active_count();
    u32 devices = block_active_count();
    struct kernel_object *root = vfs_root();
    int valid = root != 0;
    struct adytumfs_superblock super;
    u64 data_inode = 0;
    u64 other_inode = 0;
    struct adytumfs_inode in;
    struct kernel_object *dev = 0;
    struct kernel_object *point = 0;
    u64 claimed_block = 0;
    u64 record_block = 0;
    u64 table = 0;
    u64 leak = 0;
    u32 record_at = 0;
    u32 slot = 0;
    int ok = 0;

    // A forged extent into the metadata prefix would alias the superblock or
    // the inode table through a regular file.
    ok = !integrity_stage(root, &dev, &point, &super, &data_inode,
                          &other_inode) &&
        !adytumfs_inode_read(dev, &super, data_inode, &in) && in.blocks == 1;
    if (ok) in.direct[0].start_block = 0;
    ok = ok && !adytumfs_inode_write(dev, &super, data_inode, &in) &&
        !integrity_commit(dev) && vfs_mount_adytumfs(point, dev) < 0;
    valid = valid && ok;
    integrity_cleanup(root, point, dev);

    // The backup superblock block is never allocatable either.
    ok = !integrity_stage(root, &dev, &point, &super, &data_inode,
                          &other_inode) &&
        !adytumfs_inode_read(dev, &super, data_inode, &in);
    if (ok) in.direct[0].start_block = super.total_blocks - 1;
    ok = ok && !adytumfs_inode_write(dev, &super, data_inode, &in) &&
        !integrity_commit(dev) && vfs_mount_adytumfs(point, dev) < 0;
    valid = valid && ok;
    integrity_cleanup(root, point, dev);

    // Two files sharing one block would write through one allocation behind
    // each other's backs.
    ok = !integrity_stage(root, &dev, &point, &super, &data_inode,
                          &other_inode) &&
        !adytumfs_inode_read(dev, &super, data_inode, &in);
    if (ok) claimed_block = in.direct[0].start_block;
    ok = ok && !adytumfs_inode_read(dev, &super, other_inode, &in);
    if (ok) in.direct[0].start_block = claimed_block;
    ok = ok && !adytumfs_inode_write(dev, &super, other_inode, &in) &&
        !integrity_commit(dev) && vfs_mount_adytumfs(point, dev) < 0;
    valid = valid && ok;
    integrity_cleanup(root, point, dev);

    // A claim on a bit the bitmap says is free means the allocator would hand
    // that block to a second owner.
    ok = !integrity_stage(root, &dev, &point, &super, &data_inode,
                          &other_inode) &&
        !adytumfs_inode_read(dev, &super, data_inode, &in) &&
        !adytumfs_block_read(dev, super.block_bitmap_start,
                             adytumfs_probe_block);
    if (ok)
        adytumfs_probe_block[in.direct[0].start_block / 8] &=
            (u8)~(1u << (in.direct[0].start_block % 8));
    ok = ok && !adytumfs_block_write(dev, super.block_bitmap_start,
                                     adytumfs_probe_block) &&
        !integrity_commit(dev) && vfs_mount_adytumfs(point, dev) < 0;
    valid = valid && ok;
    integrity_cleanup(root, point, dev);

    // An allocated bit no extent owns is a leak the volume must not paper
    // over.
    ok = !integrity_stage(root, &dev, &point, &super, &data_inode,
                          &other_inode) &&
        !adytumfs_block_read(dev, super.block_bitmap_start,
                             adytumfs_probe_block);
    if (ok) {
        for (leak = super.data_start; leak < super.total_blocks - 1; leak++)
            if (!(adytumfs_probe_block[leak / 8] &
                  (u8)(1u << (leak % 8))))
                break;
        ok = leak < super.total_blocks - 1;
        if (ok)
            adytumfs_probe_block[leak / 8] |= (u8)(1u << (leak % 8));
    }
    ok = ok && !adytumfs_block_write(dev, super.block_bitmap_start,
                                     adytumfs_probe_block) &&
        !integrity_commit(dev) && vfs_mount_adytumfs(point, dev) < 0;
    valid = valid && ok;
    integrity_cleanup(root, point, dev);

    // The block count must equal the mapped extents, or reads clamp to a lie.
    ok = !integrity_stage(root, &dev, &point, &super, &data_inode,
                          &other_inode) &&
        !adytumfs_inode_read(dev, &super, data_inode, &in);
    if (ok) in.blocks = 2;
    ok = ok && !adytumfs_inode_write(dev, &super, data_inode, &in) &&
        !integrity_commit(dev) && vfs_mount_adytumfs(point, dev) < 0;
    valid = valid && ok;
    integrity_cleanup(root, point, dev);

    // Extents are packed from the front; an entry behind the zero length
    // terminator is a layout this format never writes.
    ok = !integrity_stage(root, &dev, &point, &super, &data_inode,
                          &other_inode) &&
        !adytumfs_inode_read(dev, &super, data_inode, &in);
    if (ok) {
        in.direct[2].start_block = in.direct[0].start_block;
        in.direct[2].length = 1;
        in.blocks = 2;
    }
    ok = ok && !adytumfs_inode_write(dev, &super, data_inode, &in) &&
        !integrity_commit(dev) && vfs_mount_adytumfs(point, dev) < 0;
    valid = valid && ok;
    integrity_cleanup(root, point, dev);

    // The mount root is a directory; anything else would have file data
    // parsed as directory records.
    ok = !integrity_stage(root, &dev, &point, &super, &data_inode,
                          &other_inode) &&
        !adytumfs_inode_read(dev, &super, super.root_inode, &in);
    if (ok) in.mode = ADYTUMFS_MODE_REG | 0755u;
    ok = ok && !adytumfs_inode_write(dev, &super, super.root_inode, &in) &&
        !integrity_commit(dev) && vfs_mount_adytumfs(point, dev) < 0;
    valid = valid && ok;
    integrity_cleanup(root, point, dev);

    // The free block counter must state what a bitmap scan finds.
    ok = !integrity_stage(root, &dev, &point, &super, &data_inode,
                          &other_inode);
    if (ok) super.free_blocks += 1;
    if (ok) adytumfs_super_pack(adytumfs_probe_block, &super);
    ok = ok && !adytumfs_block_write(dev, 0, adytumfs_probe_block) &&
        !integrity_commit(dev) && vfs_mount_adytumfs(point, dev) < 0;
    valid = valid && ok;
    integrity_cleanup(root, point, dev);

    // So must the free inode counter.
    ok = !integrity_stage(root, &dev, &point, &super, &data_inode,
                          &other_inode);
    if (ok) super.free_inodes -= 1;
    if (ok) adytumfs_super_pack(adytumfs_probe_block, &super);
    ok = ok && !adytumfs_block_write(dev, 0, adytumfs_probe_block) &&
        !integrity_commit(dev) && vfs_mount_adytumfs(point, dev) < 0;
    valid = valid && ok;
    integrity_cleanup(root, point, dev);

    // A volume sized beyond the v1 table cap is one this format never wrote.
    ok = !integrity_stage(root, &dev, &point, &super, &data_inode,
                          &other_inode);
    if (ok) {
        super.inode_table_blocks = ADYTUMFS_INODE_TABLE_BLOCKS_MAX + 1;
        super.inode_count =
            super.inode_table_blocks * ADYTUMFS_INODES_PER_BLOCK;
        super.data_start =
            super.inode_table_start + super.inode_table_blocks;
        adytumfs_super_pack(adytumfs_probe_block, &super);
    }
    ok = ok && !adytumfs_block_write(dev, 0, adytumfs_probe_block) &&
        !integrity_commit(dev) && vfs_mount_adytumfs(point, dev) < 0;
    valid = valid && ok;
    integrity_cleanup(root, point, dev);

    // An orphaned file still owns its blocks, so a consistent volume with an
    // unreachable inode must keep mounting.
    ok = !integrity_stage(root, &dev, &point, &super, &data_inode,
                          &other_inode) &&
        !policy_find_record(dev, &super, "data", 4, &record_block,
                            &record_at);
    if (ok) adytumfs_write_le64(adytumfs_probe_block + record_at, 0);
    ok = ok && !policy_commit(dev, record_block) &&
        !vfs_mount_adytumfs(point, dev);
    struct kernel_object *disk = ok ? vfs_lookup(root, "integrity") : 0;
    struct kernel_object *kept = disk ? vfs_lookup(disk, "other") : 0;
    ok = ok && disk && kept && !vfs_lookup(disk, "data");
    if (kept) object_release(kept);
    if (disk) object_release(disk);
    ok = ok && !vfs_unmount(point);
    valid = valid && ok;
    integrity_cleanup(root, point, dev);

    // A used slot with a broken checksum means the table itself is lying,
    // reachable or not.
    ok = !integrity_stage(root, &dev, &point, &super, &data_inode,
                          &other_inode) &&
        !policy_find_record(dev, &super, "data", 4, &record_block,
                            &record_at);
    if (ok) adytumfs_write_le64(adytumfs_probe_block + record_at, 0);
    table = super.inode_table_start + data_inode / ADYTUMFS_INODES_PER_BLOCK;
    slot = (u32)(data_inode % ADYTUMFS_INODES_PER_BLOCK) *
           ADYTUMFS_INODE_SIZE;
    ok = ok && !adytumfs_block_write(dev, record_block,
                                     adytumfs_probe_block) &&
        !adytumfs_block_read(dev, table, adytumfs_probe_block);
    if (ok) adytumfs_probe_block[slot + 100] ^= 0xffu;
    ok = ok && !adytumfs_block_write(dev, table, adytumfs_probe_block) &&
        !integrity_commit(dev) && vfs_mount_adytumfs(point, dev) < 0;
    valid = valid && ok;
    integrity_cleanup(root, point, dev);

    if (root) object_release(root);
    valid = valid && object_active_count() == objects &&
        vfs_node_active_count() == nodes &&
        vfs_file_active_count() == files &&
        vfs_mount_active_count() == mounts &&
        block_active_count() == devices;
    return valid ? 0 : -1;
}
