#include "types.h"
#include "object.h"
#include "block.h"
#include "blockfs.h"
#include "vfs.h"

int test_blockfs64(void) {
    u32 objects = object_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    u32 mounts = vfs_mount_active_count();
    u32 devices = block_active_count();
    struct kernel_object *root = vfs_root();
    struct kernel_object *mnt = root ?
        vfs_create(root, "disk", VFS_NODE_DIRECTORY) : 0;
    struct kernel_object *dev = block_create(64, 0);
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
