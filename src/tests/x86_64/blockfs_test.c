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
        vfs_create(disk, "hello", VFS_NODE_REGULAR) : 0;
    struct kernel_object *opened = node ? vfs_open(node) : 0;
    struct vfs_node_info info;
    valid = valid && disk && node && opened &&
        !vfs_write(opened, 0, payload, sizeof(payload), &transferred) &&
        transferred == sizeof(payload) &&
        !vfs_read(opened, 0, received, sizeof(received), &transferred) &&
        transferred == sizeof(received) &&
        !vfs_stat(opened, &info) &&
        info.filesystem == VFS_FILESYSTEM_BLOCKFS &&
        info.size == sizeof(payload) && !info.readonly;
    for (u32 i = 0; i < 16; i++)
        if (received[i] != payload[i]) valid = 0;
    valid = valid && !vfs_truncate(opened, 4) &&
        !vfs_stat(node, &info) && info.size == 4 &&
        !vfs_read(opened, 0, received, sizeof(received), &transferred) &&
        transferred == 4 &&
        !vfs_unlink(disk, "hello");
    if (opened) object_release(opened);
    if (node) object_release(node);
    if (disk) object_release(disk);
    if (mnt && vfs_unmount(mnt)) valid = 0;
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
