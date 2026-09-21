#include "types.h"
#include "object.h"
#include "task.h"
#include "vfs.h"
#include "posix_fd.h"
#include "posix_profile.h"
#include "posix_vfs.h"
#include "tests64.h"

int test_posix_vfs64(struct task *owner, struct task *child) {
    u32 objects = object_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    u32 descriptors = posix_fd_active_count();
    u32 ofds = posix_ofd_active_count();
    struct kernel_object *root = vfs_root();
    struct kernel_object *directory = root ?
        vfs_create(root, "posix-api", VFS_NODE_DIRECTORY) : 0;
    int denied = directory ? posix_vfs_open(
        child, "/posix-api/denied", POSIX_OPEN_RDWR | POSIX_OPEN_CREAT, 0600) : -1;
    int admitted = !posix_profile_admit(owner);
    int state = admitted ? posix_vfs_open(
        owner, "/posix-api/state", POSIX_OPEN_RDWR | POSIX_OPEN_CREAT |
        POSIX_OPEN_CLOEXEC, 0600) : -1;
    struct vfs_node_info info;
    u32 transferred = 0;
    u8 payload[3] = { 'a', 'b', 'c' };
    u8 received[3];
    u32 position = 0;
    int valid = root && directory && denied == POSIX_VFS_EACCES && admitted &&
        state == 0;
    valid &= !posix_vfs_stat_path(owner, "/posix-api/state", &info);
    valid &= info.type == VFS_NODE_REGULAR && info.mode == 0600;
    valid &= !posix_fd_get_cloexec(owner, state, &position) && position;
    valid &= !posix_fd_write(owner, state, payload, sizeof(payload),
                             &transferred) && transferred == sizeof(payload);
    valid &= !posix_vfs_truncate_path(owner, "/posix-api/state", 1);
    valid &= !posix_fd_stat(owner, state, &info) && info.size == 1;
    valid &= !posix_fd_seek(owner, state, 0, 0, &position);
    valid &= !posix_fd_read(owner, state, received, sizeof(received),
                            &transferred) && transferred == 1 &&
        received[0] == payload[0];
    valid &= posix_vfs_open(owner, "/posix-api/state", POSIX_OPEN_RDONLY |
                            POSIX_OPEN_TRUNC, 0) == POSIX_VFS_EINVAL;

    int readonly = posix_vfs_open(owner, "/posix-api/readonly",
                                  POSIX_OPEN_RDONLY | POSIX_OPEN_CREAT, 0400);
    valid &= readonly == 1;
    if (readonly >= 0) valid &= !posix_fd_close(owner, readonly);
    int append = posix_vfs_open(owner, "/posix-api/state", POSIX_OPEN_RDWR |
                                POSIX_OPEN_APPEND, 0);
    valid &= append == 1;
    valid &= !posix_fd_write(owner, append, payload + 1, 1, &transferred) &&
        transferred == 1;
    if (append >= 0) valid &= !posix_fd_close(owner, append);
    valid &= posix_vfs_open(owner, "/posix-api/readonly", POSIX_OPEN_WRONLY,
                            0) == POSIX_VFS_EACCES;
    valid &= posix_vfs_open(owner, "/boot/init64", POSIX_OPEN_WRONLY, 0) ==
        POSIX_VFS_EROFS;
    valid &= !posix_vfs_mkdir(owner, "/posix-api/dir", 0700);
    valid &= !posix_vfs_mkdir(owner, "/posix-api/sealed", 0600);
    valid &= !posix_vfs_mkdir(owner, "/posix-api/no-write", 0500);
    valid &= posix_vfs_open(owner, "/posix-api/sealed/item",
                            POSIX_OPEN_WRONLY | POSIX_OPEN_CREAT, 0600) ==
        POSIX_VFS_EACCES;
    valid &= posix_vfs_open(owner, "/posix-api/no-write/item",
                            POSIX_OPEN_WRONLY | POSIX_OPEN_CREAT, 0600) ==
        POSIX_VFS_EACCES;
    valid &= posix_vfs_open(owner, "/posix-api/zero",
                            POSIX_OPEN_WRONLY | POSIX_OPEN_CREAT, 0000) ==
        POSIX_VFS_EACCES;
    valid &= posix_vfs_stat_path(owner, "/posix-api/zero", &info) ==
        POSIX_PROFILE_ENOENT;
    valid &= posix_vfs_unlink(owner, "/posix-api/dir") == POSIX_VFS_EISDIR;
    valid &= posix_vfs_rmdir(owner, "/posix-api/state") == POSIX_VFS_ENOTDIR;
    valid &= !posix_vfs_rmdir(owner, "/posix-api/dir");
    valid &= !posix_vfs_unlink(owner, "/posix-api/state");
    valid &= !posix_vfs_unlink(owner, "/posix-api/readonly");
    valid &= !posix_vfs_rmdir(owner, "/posix-api/sealed");
    valid &= !posix_vfs_rmdir(owner, "/posix-api/no-write");
    valid &= !posix_vfs_rmdir(owner, "/posix-api");

    if (state >= 0) valid &= !posix_fd_close(owner, state);
    posix_profile_release(owner);
    if (directory) object_release(directory);
    if (root) object_release(root);
    valid &= !posix_profile_admitted(owner) &&
        object_active_count() == objects && vfs_node_active_count() == nodes &&
        vfs_file_active_count() == files &&
        posix_fd_active_count() == descriptors && posix_ofd_active_count() == ofds;
    return valid ? 0 : -1;
}
