#include "types.h"
#include "object.h"
#include "task.h"
#include "vfs.h"
#include "posix_fd.h"
#include "tests64.h"

int test_posix_fd64(struct task *owner, struct task *child) {
    u32 objects = object_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    u32 descriptors = posix_fd_active_count();
    u32 ofd_count = posix_ofd_active_count();
    int valid = !descriptors && !ofd_count;
    int filled[POSIX_FD_MAX];
    for (u32 index = 0; index < POSIX_FD_MAX; index++) filled[index] = -1;

    struct kernel_object *root = vfs_root();
    struct kernel_object *directory = root ?
        vfs_create(root, "posix", VFS_NODE_DIRECTORY) : 0;
    struct kernel_object *node = directory ?
        vfs_create(directory, "state", VFS_NODE_REGULAR) : 0;
    struct kernel_object *file = node ? vfs_open(node) : 0;
    int first = file ? posix_fd_install_vfs(
        owner, file, POSIX_FD_ACCESS_READ | POSIX_FD_ACCESS_WRITE, 0, 0) : -1;
    if (file) {
        object_release(file);
        file = 0;
    }
    int duplicate = first >= 0 ? posix_fd_dup(owner, first) : -1;
    file = node ? vfs_open(node) : 0;
    int displaced = file ? posix_fd_install_vfs(
        owner, file, POSIX_FD_ACCESS_READ, 0, 0) : -1;
    if (file) {
        object_release(file);
        file = 0;
    }
    int duplicate_two = displaced >= 0 ?
        posix_fd_dup2(owner, first, displaced) : -1;
    u8 payload[5] = { 'A', 'B', 'C', 'D', 'E' };
    u8 received[5];
    u32 transferred = 0;
    u32 position = 0;
    struct vfs_node_info info;
    valid = valid && root && directory && node && first == 0 && duplicate == 1 &&
        displaced == 2 && duplicate_two == displaced &&
        !posix_fd_write(owner, first, payload, 3, &transferred) &&
        transferred == 3 && !posix_fd_seek(owner, duplicate, -2, 1, &position) &&
        position == 1 && !posix_fd_read(owner, first, received, 1, &transferred) &&
        transferred == 1 && received[0] == payload[1] &&
        !posix_fd_seek(owner, duplicate, 1, 1, &position) && position == 3 &&
        !posix_fd_stat(owner, first, &info) && info.size == 3 &&
        !posix_fd_read(owner, duplicate, received, sizeof(received),
                       &transferred) && !transferred;

    file = node ? vfs_open(node) : 0;
    int child_guard = file ? posix_fd_install_vfs(
        child, file, POSIX_FD_ACCESS_READ, 0, 0) : -1;
    if (file) {
        object_release(file);
        file = 0;
    }
    u32 before_failed_fork = posix_fd_active_count();
    valid = valid && child_guard == 0 && posix_fd_fork(owner, child) < 0 &&
        posix_fd_active_count() == before_failed_fork &&
        !posix_fd_read(child, child_guard, received, 1, &transferred) &&
        transferred == 1 && received[0] == payload[0] &&
        !posix_fd_close(child, child_guard) && !posix_fd_fork(owner, child) &&
        !posix_fd_write(child, first, payload + 3, 1, &transferred) &&
        transferred == 1 && !posix_fd_read(owner, duplicate, received,
                                            sizeof(received), &transferred) &&
        !transferred;

    file = node ? vfs_open(node) : 0;
    int cloexec = file ? posix_fd_install_vfs(
        owner, file, POSIX_FD_ACCESS_READ, 0, POSIX_FD_CLOEXEC) : -1;
    if (file) {
        object_release(file);
        file = 0;
    }
    int preserved = cloexec >= 0 ? posix_fd_dup(owner, cloexec) : -1;
    u32 enabled = 0;
    valid = valid && cloexec >= 0 && preserved >= 0 &&
        !posix_fd_get_cloexec(owner, cloexec, &enabled) && enabled &&
        !posix_fd_get_cloexec(owner, preserved, &enabled) && !enabled &&
        !posix_fd_set_cloexec(owner, preserved, 1) &&
        !posix_fd_get_cloexec(owner, preserved, &enabled) && enabled &&
        !posix_fd_set_cloexec(owner, preserved, 0);
    posix_fd_close_cloexec(owner);
    valid = valid && posix_fd_get_cloexec(owner, cloexec, &enabled) < 0 &&
        !posix_fd_read(owner, preserved, received, 1, &transferred) &&
        transferred == 1 && received[0] == payload[0];

    file = node ? vfs_open(node) : 0;
    int appended = file ? posix_fd_install_vfs(
        owner, file, POSIX_FD_ACCESS_WRITE, POSIX_FD_APPEND, 0) : -1;
    if (file) {
        object_release(file);
        file = 0;
    }
    valid = valid && appended >= 0 &&
        !posix_fd_write(owner, appended, payload + 4, 1, &transferred) &&
        transferred == 1;
    file = node ? vfs_open(node) : 0;
    valid = valid && file && !vfs_read(file, 0, received, sizeof(received),
                                       &transferred) &&
        transferred == sizeof(received) && received[3] == payload[3] &&
        received[4] == payload[4];
    if (file) {
        object_release(file);
        file = 0;
    }

    file = node ? vfs_open(node) : 0;
    int revoked = file ? posix_fd_install_vfs(
        owner, file, POSIX_FD_ACCESS_READ, 0, 0) : -1;
    valid = valid && revoked >= 0 && !handle_revoke_object(file) &&
        posix_fd_read(owner, revoked, received, 1, &transferred) < 0;
    if (file) {
        object_release(file);
        file = 0;
    }

    struct task *cleanup_task = task_alloc_slot();
    file = cleanup_task && node ? vfs_open(node) : 0;
    int cleanup_descriptor = file ? posix_fd_install_vfs(
        cleanup_task, file, POSIX_FD_ACCESS_READ, 0, 0) : -1;
    if (file) {
        object_release(file);
        file = 0;
    }
    if (cleanup_task) task_mark_zombie(cleanup_task, 0);
    valid = valid && cleanup_task && cleanup_descriptor == 0 &&
        posix_fd_read(cleanup_task, cleanup_descriptor, received, 1,
                      &transferred) < 0;
    if (cleanup_task) task_free_slot(cleanup_task);

    posix_fd_close_all(owner);
    posix_fd_close_all(child);
    valid = valid && posix_fd_active_count() == descriptors &&
        posix_ofd_active_count() == ofd_count &&
        vfs_file_active_count() == files;

    for (u32 index = 0; index < POSIX_FD_MAX; index++) {
        file = node ? vfs_open(node) : 0;
        if (!file) {
            valid = 0;
            continue;
        }
        filled[index] = posix_fd_install_vfs(
            owner, file, POSIX_FD_ACCESS_READ, 0, 0);
        object_release(file);
        file = 0;
        if (filled[index] != (int)index) valid = 0;
    }
    valid = valid && posix_fd_active_count() == descriptors + POSIX_FD_MAX &&
        posix_ofd_active_count() == ofd_count + POSIX_OFD_MAX &&
        vfs_file_active_count() == files + VFS_FILE_MAX &&
        posix_fd_dup(owner, filled[0]) < 0;
    file = node ? vfs_open(node) : 0;
    valid = valid && !file;
    if (file) object_release(file);
    file = 0;
    if (filled[7] >= 0 && posix_fd_close(owner, filled[7])) valid = 0;
    file = node ? vfs_open(node) : 0;
    int reused = file ? posix_fd_install_vfs(
        owner, file, POSIX_FD_ACCESS_READ, 0, 0) : -1;
    if (file) {
        object_release(file);
        file = 0;
    }
    valid = valid && reused == 7 && !posix_fd_close(owner, reused);
    posix_fd_close_all(owner);

    if (node && vfs_unlink(directory, "state")) valid = 0;
    if (directory && vfs_unlink(root, "posix")) valid = 0;
    if (node) object_release(node);
    if (directory) object_release(directory);
    if (root) object_release(root);
    valid = valid && object_active_count() == objects &&
        vfs_node_active_count() == nodes &&
        vfs_file_active_count() == files &&
        posix_fd_active_count() == descriptors &&
        posix_ofd_active_count() == ofd_count;
    return valid ? 0 : -1;
}
