#include "types.h"
#include "object.h"
#include "vfs.h"
#include "firmware.h"

int test_vfs64(void) {
    u32 objects = object_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    u32 mounts = vfs_mount_active_count();
    struct kernel_object *root = vfs_root();
    struct kernel_object *mount = vfs_mount_root();
    struct kernel_object *directory = root ?
        vfs_create(root, "etc", VFS_NODE_DIRECTORY) : 0;
    struct kernel_object *node = directory ?
        vfs_create_mode(directory, "config", VFS_NODE_REGULAR, 0640) : 0;
    struct kernel_object *invalid_mode = directory ?
        vfs_create_mode(directory, "bad-mode", VFS_NODE_REGULAR, 01000) : 0;
    struct kernel_object *duplicate = directory ?
        vfs_create(directory, "config", VFS_NODE_REGULAR) : 0;
    struct kernel_object *invalid = root ?
        vfs_create(root, "../escape", VFS_NODE_REGULAR) : 0;
    struct kernel_object *lookup = directory ?
        vfs_lookup(directory, "config") : 0;
    struct kernel_object *opened = node ? vfs_open(node) : 0;
    u8 payload[32];
    u8 received[32];
    for (u32 index = 0; index < sizeof(payload); index++) payload[index] = (u8)index;
    u32 transferred = 0;
    u32 appended_position = 0;
    u8 append_first[2] = { 'a', 'b' };
    u8 append_second[2] = { 'c', 'd' };
    struct kernel_object *boot_directory = root ?
        vfs_resolve(root, "/boot") : 0;
    struct kernel_object *boot_node = root ?
        vfs_resolve(root, "/boot/init64") : 0;
    struct kernel_object *boot_parent = boot_directory ?
        vfs_resolve(boot_directory, "..") : 0;
    struct kernel_object *boot_file = boot_node ? vfs_open(boot_node) : 0;
    u32 firmware_size = 0;
    struct kernel_object *firmware_file = firmware_open("init64", &firmware_size);
    struct vfs_node_info boot_info;
    int boot_valid = boot_directory && boot_node && boot_parent == root &&
        boot_file && firmware_file && firmware_size > 4 &&
        !firmware_open("../init64", &firmware_size) &&
        !vfs_stat(boot_file, &boot_info) &&
        boot_info.filesystem == VFS_FILESYSTEM_BOOTFS && boot_info.readonly &&
        boot_info.mode == VFS_MODE_REGULAR_READONLY &&
        !vfs_read(boot_file, 0, received, 4, &transferred) && transferred == 4 &&
        received[0] == 0x7F && received[1] == 'E' &&
        received[2] == 'L' && received[3] == 'F' &&
        vfs_write(boot_file, 0, payload, 1, &transferred) < 0 &&
        vfs_append(boot_file, payload, 1, &transferred,
                   &appended_position) < 0 &&
        vfs_truncate(boot_file, 0) < 0 &&
        !vfs_create(boot_directory, "mutable", VFS_NODE_REGULAR) &&
        vfs_unlink(root, "boot") < 0;
    int valid = boot_valid && root && mount && directory && node &&
        !duplicate && !invalid && !invalid_mode && lookup == node && opened &&
        !vfs_write(opened, 0, payload, sizeof(payload), &transferred) &&
        transferred == sizeof(payload) &&
        !vfs_read(opened, 0, received, sizeof(received), &transferred) &&
        transferred == sizeof(received);
    for (u32 index = 0; index < sizeof(payload); index++)
        if (received[index] != payload[index]) valid = 0;
    struct vfs_node_info info;
    valid = valid && !vfs_stat(opened, &info) &&
        info.type == VFS_NODE_REGULAR && info.size == sizeof(payload) &&
        info.mode == 0640 && info.linked && !vfs_truncate(opened, 8) &&
        !vfs_stat(node, &info) && info.size == 8 &&
        !vfs_append(opened, append_first, sizeof(append_first), &transferred,
                    &appended_position) &&
        transferred == sizeof(append_first) && appended_position == 10 &&
        !vfs_truncate(opened, 9) &&
        !vfs_append(opened, append_second, sizeof(append_second), &transferred,
                    &appended_position) &&
        transferred == sizeof(append_second) && appended_position == 11 &&
        !vfs_stat(node, &info) && info.size == 11 &&
        vfs_unlink(root, "etc") < 0 &&
        !vfs_unlink(directory, "config") &&
        !vfs_stat(opened, &info) && !info.linked &&
        !vfs_read(opened, 0, received, sizeof(received), &transferred) &&
        transferred == 11 && received[8] == append_first[0] &&
        received[9] == append_second[0] && received[10] == append_second[1] &&
        !vfs_unlink(root, "etc");
    struct kernel_object *usr = root ?
        vfs_create_path(root, "/usr", VFS_NODE_DIRECTORY) : 0;
    struct kernel_object *lib = root ?
        vfs_create_path(root, "/usr/lib", VFS_NODE_DIRECTORY) : 0;
    struct kernel_object *mich = root ?
        vfs_create_path(root, "/usr/lib/mich", VFS_NODE_DIRECTORY) : 0;
    struct kernel_object *path_file = root ?
        vfs_create_path(root, "/usr/lib/mich/config", VFS_NODE_REGULAR) : 0;
    struct kernel_object *absolute = root ? vfs_resolve(
        root, "///usr//lib/./mich/../mich/config") : 0;
    struct kernel_object *relative = mich ?
        vfs_resolve(mich, "../mich/config") : 0;
    struct kernel_object *clamped = root ?
        vfs_resolve(root, "/../../usr") : 0;
    struct kernel_object *trailing = root ?
        vfs_resolve(root, "/usr/lib/mich/config/") : 0;
    char deep[VFS_PATH_MAX];
    u32 deep_length = 0;
    for (u32 component = 0; component < VFS_PATH_COMPONENT_MAX + 1;
         component++) {
        deep[deep_length++] = '/';
        deep[deep_length++] = '.';
    }
    deep[deep_length] = 0;
    valid = valid && usr && lib && mich && path_file &&
        absolute == path_file && relative == path_file && clamped == usr &&
        !trailing && !vfs_resolve(root, deep) &&
        !vfs_unlink_path(root, "/usr/lib/mich/config") &&
        !vfs_unlink_path(root, "/usr/lib/mich") &&
        !vfs_unlink_path(root, "/usr/lib") &&
        !vfs_unlink_path(root, "/usr");
    if (absolute) object_release(absolute);
    if (relative) object_release(relative);
    if (clamped) object_release(clamped);
    if (path_file) object_release(path_file);
    if (mich) object_release(mich);
    if (lib) object_release(lib);
    if (usr) object_release(usr);
    for (u32 iteration = 0; iteration < 4096; iteration++) {
        char mutation[VFS_PATH_MAX];
        u32 mutation_length = 1 + (iteration % 96);
        for (u32 index = 0; index < mutation_length; index++) {
            u32 value = (iteration * 17 + index * 13) % 7;
            mutation[index] = value == 0 ? '/' : value == 1 ? '.' :
                (char)('a' + value - 2);
        }
        mutation[mutation_length] = 0;
        struct kernel_object *resolved = vfs_resolve(root, mutation);
        if (resolved) object_release(resolved);
    }
    if (firmware_file) object_release(firmware_file);
    if (boot_file) object_release(boot_file);
    if (boot_parent) object_release(boot_parent);
    if (boot_node) object_release(boot_node);
    if (boot_directory) object_release(boot_directory);
    if (opened) object_release(opened);
    if (lookup) object_release(lookup);
    if (node) object_release(node);
    if (directory) object_release(directory);
    if (mount) object_release(mount);
    if (root) object_release(root);
    valid = valid && object_active_count() == objects &&
        vfs_node_active_count() == nodes &&
        vfs_file_active_count() == files &&
        vfs_mount_active_count() == mounts;
    return valid ? 0 : -1;
}
