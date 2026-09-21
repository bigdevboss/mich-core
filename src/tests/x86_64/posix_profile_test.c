#include "types.h"
#include "object.h"
#include "task.h"
#include "vfs.h"
#include "posix_profile.h"
#include "tests64.h"

static int paths_equal(const char *left, const char *right) {
    for (u32 index = 0; index < VFS_PATH_MAX; index++) {
        if (left[index] != right[index]) return 0;
        if (!left[index]) return 1;
    }
    return 0;
}

int test_posix_profile64(struct task *owner, struct task *child) {
    u32 objects = object_active_count();
    u32 nodes = vfs_node_active_count();
    int boot_owner = posix_profile_admitted(owner);
    int boot_child = posix_profile_admitted(child);
    posix_profile_release(owner);
    posix_profile_release(child);
    struct kernel_object *root = vfs_root();
    struct kernel_object *directory = root ?
        vfs_create(root, "profile", VFS_NODE_DIRECTORY) : 0;
    struct kernel_object *work = directory ?
        vfs_create(directory, "work", VFS_NODE_DIRECTORY) : 0;
    struct kernel_object *file = work ?
        vfs_create(work, "file", VFS_NODE_REGULAR) : 0;
    struct kernel_object *locked = directory ?
        vfs_create_mode(directory, "locked", VFS_NODE_DIRECTORY, 0600) : 0;
    struct kernel_object *resolved = 0;
    char path[VFS_PATH_MAX];
    int valid = root && directory && work && file && locked && boot_owner &&
        boot_child && !posix_profile_admitted(owner) &&
        posix_profile_chdir(owner, "/profile") == POSIX_PROFILE_EACCES &&
        !posix_profile_admit(owner) && posix_profile_admitted(owner) &&
        posix_profile_vfs_authorized(owner) &&
        !posix_profile_getcwd(owner, path) && paths_equal(path, "/") &&
        !posix_profile_chdir(owner, "/profile/work") &&
        !posix_profile_getcwd(owner, path) &&
        paths_equal(path, "/profile/work") &&
        !posix_profile_chdir(owner, "../work/.") &&
        !posix_profile_getcwd(owner, path) &&
        paths_equal(path, "/profile/work") &&
        posix_profile_chdir(owner, "file") == POSIX_PROFILE_ENOTDIR &&
        posix_profile_chdir(owner, "/profile/locked") == POSIX_PROFILE_EACCES &&
        !posix_profile_fork(owner, child) && posix_profile_admitted(child) &&
        posix_profile_vfs_authorized(child) && !child->capabilities &&
        !posix_profile_getcwd(child, path) && paths_equal(path, "/profile/work");

    posix_profile_release(child);
    valid = valid && !posix_profile_admitted(child) &&
        !vfs_unlink(work, "file") && !vfs_unlink(directory, "work") &&
        posix_profile_getcwd(owner, path) == POSIX_PROFILE_ENOENT &&
        posix_profile_resolve(owner, ".", &resolved) == POSIX_PROFILE_ENOENT &&
        !posix_profile_resolve(owner, "/profile", &resolved) && resolved &&
        !posix_profile_chdir(owner, "/") &&
        !posix_profile_getcwd(owner, path) && paths_equal(path, "/");
    if (resolved) {
        object_release(resolved);
        resolved = 0;
    }

    struct task *cleanup = task_alloc_slot();
    valid = valid && cleanup && !posix_profile_admit(cleanup) &&
        posix_profile_admitted(cleanup);
    if (cleanup) task_mark_zombie(cleanup, 0);
    valid = valid && cleanup && !posix_profile_admitted(cleanup);
    if (cleanup) task_free_slot(cleanup);

    posix_profile_release(owner);
    if (file) object_release(file);
    if (work) object_release(work);
    if (directory && vfs_unlink(directory, "locked")) valid = 0;
    if (locked) object_release(locked);
    if (directory && vfs_unlink(root, "profile")) valid = 0;
    if (directory) object_release(directory);
    if (root) object_release(root);
    valid = valid && object_active_count() == objects &&
        vfs_node_active_count() == nodes;
    return valid ? 0 : -1;
}
