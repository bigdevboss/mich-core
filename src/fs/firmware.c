#include "firmware.h"
#include "vfs.h"

static int valid_name(const char *name) {
    if (!name || !name[0]) return 0;
    for (u32 index = 0; index < VFS_NAME_MAX; index++) {
        if (name[index] == '/') return 0;
        if (!name[index]) return 1;
    }
    return 0;
}

struct kernel_object *firmware_open(const char *name, u32 *size) {
    if (!valid_name(name) || !size) return 0;
    char path[VFS_PATH_MAX];
    for (u32 index = 0; index < VFS_PATH_MAX; index++) path[index] = 0;
    const char prefix[] = "/boot/";
    u32 offset = 0;
    while (prefix[offset]) {
        path[offset] = prefix[offset];
        offset++;
    }
    for (u32 index = 0; name[index]; index++) path[offset++] = name[index];
    struct kernel_object *root = vfs_root();
    struct kernel_object *node = root ? vfs_resolve(root, path) : 0;
    if (root) object_release(root);
    struct vfs_node_info info;
    if (!node || vfs_stat(node, &info) ||
        info.type != VFS_NODE_REGULAR ||
        info.filesystem != VFS_FILESYSTEM_BOOTFS || !info.readonly) {
        if (node) object_release(node);
        return 0;
    }
    struct kernel_object *file = vfs_open(node);
    object_release(node);
    if (!file) return 0;
    *size = info.size;
    return file;
}
