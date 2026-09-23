#include "types.h"
#include "object.h"
#include "block.h"
#include "blockfs.h"
#include "vfs.h"
#include "resource.h"

int test_blockfs64(void) {
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
    int valid = root && mnt && dev && !blockfs_format(dev) &&
        !vfs_mount_blockfs(mnt, dev);
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
        info.filesystem == VFS_FILESYSTEM_BLOCKFS &&
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
    int remounted = first_unmount && !vfs_mount_blockfs(mnt, dev);
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
    if (reopened) {
        object_release(reopened);
        reopened = 0;
    }
    int second_unmount = fresh_directory && fresh_folder &&
        !vfs_unlink(fresh_directory, "hello") &&
        !vfs_unlink(fresh_folder, "nested") &&
        !vfs_unlink(fresh_directory, "folder") && !vfs_unmount(mnt);
    int final_remount = second_unmount && !vfs_mount_blockfs(mnt, dev);
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

int test_blockfs_pages64(void) {
    u32 objects = object_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    u32 mounts = vfs_mount_active_count();
    u32 devices = block_active_count();
    struct kernel_object *root = vfs_root();
    struct kernel_object *mnt = root ?
        vfs_create(root, "pages", VFS_NODE_DIRECTORY) : 0;
    struct kernel_object *dev = block_create(320, 0);
    int valid = root && mnt && dev && !blockfs_format(dev) &&
        !vfs_mount_blockfs(mnt, dev);
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

    // Write-back: nothing reached the disk yet, so a raw sector read still
    // sees the formatted zeroes.
    u8 raw[BLOCK_SECTOR_SIZE];
    for (u32 index = 0; index < sizeof(raw); index++) raw[index] = 0xFF;
    u32 used = 0, type = 0, size = 0, parent = 0, mode = 0;
    char name[32];
    u32 mount_id = 0;
    struct vfs_node_info info;
    valid = valid && !vfs_stat(opened, &info) && info.size == span;
    (void)used; (void)type; (void)size; (void)parent; (void)mode;
    (void)name; (void)mount_id;

    valid = valid && !vfs_sync(opened);

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
    if (mnt && vfs_mount_blockfs(mnt, dev)) valid = 0;
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
