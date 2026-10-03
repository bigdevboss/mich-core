#include "posix_vfs.h"
#include "posix_profile.h"
#include "posix_fd.h"
#include "task.h"
#include "object.h"

static int profile_mutation_allowed(struct task *task) {
    return posix_profile_vfs_authorized(task) ? 0 : POSIX_VFS_EACCES;
}

static int node_info(struct kernel_object *node, struct vfs_node_info *info) {
    return node && info && !vfs_stat(node, info) ? 0 : POSIX_VFS_EIO;
}

static int check_parent_mutation(struct task *task,
                                 struct kernel_object *parent) {
    struct vfs_node_info info;
    int result = node_info(parent, &info);
    if (result) return result;
    if (info.type != VFS_NODE_DIRECTORY) return POSIX_VFS_ENOTDIR;
    if (info.readonly) return POSIX_VFS_EROFS;
    // Entry creation and removal need write and search on the parent.
    if (!posix_mode_allows(&info, task, 0300u)) return POSIX_VFS_EACCES;
    return 0;
}

static int check_file_access(struct task *task,
                             const struct vfs_node_info *info, u32 access) {
    // A directory opens read-only so getdents can list it; writing through
    // one stays the EISDIR POSIX requires.
    if (info->type == VFS_NODE_DIRECTORY) {
        if (access & POSIX_FD_ACCESS_WRITE) return POSIX_VFS_EISDIR;
        if ((access & POSIX_FD_ACCESS_READ) &&
            !posix_mode_allows(info, task, 0400u))
            return POSIX_VFS_EACCES;
        return 0;
    }
    if (info->type != VFS_NODE_REGULAR) return POSIX_VFS_EIO;
    if ((access & POSIX_FD_ACCESS_READ) &&
        !posix_mode_allows(info, task, 0400u))
        return POSIX_VFS_EACCES;
    if (access & POSIX_FD_ACCESS_WRITE) {
        if (info->readonly) return POSIX_VFS_EROFS;
        if (!posix_mode_allows(info, task, 0200u))
            return POSIX_VFS_EACCES;
    }
    return 0;
}

static int access_from_flags(u32 flags, u32 *access, u32 *status,
                             u32 *descriptor_flags) {
    if (!access || !status || !descriptor_flags ||
        (flags & ~(POSIX_OPEN_ACCMODE | POSIX_OPEN_CREAT | POSIX_OPEN_TRUNC |
                   POSIX_OPEN_APPEND | POSIX_OPEN_CLOEXEC)))
        return POSIX_VFS_EINVAL;
    u32 requested = flags & POSIX_OPEN_ACCMODE;
    if (requested == POSIX_OPEN_RDONLY)
        *access = POSIX_FD_ACCESS_READ;
    else if (requested == POSIX_OPEN_WRONLY)
        *access = POSIX_FD_ACCESS_WRITE;
    else if (requested == POSIX_OPEN_RDWR)
        *access = POSIX_FD_ACCESS_READ | POSIX_FD_ACCESS_WRITE;
    else
        return POSIX_VFS_EINVAL;
    if ((flags & (POSIX_OPEN_TRUNC | POSIX_OPEN_APPEND)) &&
        !(*access & POSIX_FD_ACCESS_WRITE))
        return POSIX_VFS_EINVAL;
    *status = flags & POSIX_OPEN_APPEND ? POSIX_FD_APPEND : 0;
    *descriptor_flags = flags & POSIX_OPEN_CLOEXEC ? POSIX_FD_CLOEXEC : 0;
    return 0;
}

static int install_file(struct task *task, struct kernel_object *node,
                        u32 access, u32 status, u32 descriptor_flags,
                        int truncate) {
    struct kernel_object *file = vfs_open(node);
    if (!file) return POSIX_VFS_ENFILE;
    if (truncate && vfs_truncate(file, 0)) {
        object_release(file);
        return POSIX_VFS_EIO;
    }
    int descriptor = posix_fd_install_vfs(task, file, access, status,
                                          descriptor_flags);
    object_release(file);
    if (descriptor == POSIX_FD_TABLE_FULL) return POSIX_VFS_EMFILE;
    if (descriptor == POSIX_FD_OFD_FULL) return POSIX_VFS_ENFILE;
    return descriptor < 0 ? POSIX_VFS_ENFILE : descriptor;
}

int posix_vfs_open(struct task *task, const char *path, u32 flags, u32 mode) {
    u32 access = 0;
    u32 status = 0;
    u32 descriptor_flags = 0;
    int result = access_from_flags(flags, &access, &status, &descriptor_flags);
    if (result || (mode & ~VFS_MODE_MASK))
        return result ? result : POSIX_VFS_EINVAL;
    int mutating = (access & POSIX_FD_ACCESS_WRITE) ||
        (flags & (POSIX_OPEN_CREAT | POSIX_OPEN_TRUNC));
    if (mutating && profile_mutation_allowed(task)) return POSIX_VFS_EACCES;
    struct kernel_object *node = 0;
    struct kernel_object *parent = 0;
    char name[VFS_NAME_MAX + 1];
    int created = 0;
    result = posix_profile_resolve(task, path, &node);
    if (result == POSIX_PROFILE_ENOENT && (flags & POSIX_OPEN_CREAT)) {
        result = posix_profile_parent(task, path, &parent, name);
        if (result) return result;
        result = check_parent_mutation(task, parent);
        if (result) {
            object_release(parent);
            return result;
        }
        node = vfs_create_mode(parent, name, VFS_NODE_REGULAR,
                               mode & ~task->umask);
        if (!node) {
            node = vfs_lookup(parent, name);
            if (!node) {
                object_release(parent);
                return POSIX_VFS_ENOSPC;
            }
        } else {
            created = 1;
            if (vfs_chown(node, task->uid, task->gid)) {
                vfs_unlink(parent, name);
                object_release(node);
                node = 0;
                object_release(parent);
                return POSIX_VFS_EIO;
            }
        }
    } else if (result) {
        return result;
    }
    struct vfs_node_info info;
    result = node_info(node, &info);
    if (!result) result = check_file_access(task, &info, access);
    if (!result && (flags & POSIX_OPEN_TRUNC) && info.readonly)
        result = POSIX_VFS_EROFS;
    if (!result)
        result = install_file(task, node, access, status, descriptor_flags,
                              (flags & POSIX_OPEN_TRUNC) != 0);
    if (result < 0 && created) vfs_unlink(parent, name);
    object_release(node);
    if (parent) object_release(parent);
    return result;
}

int posix_vfs_stat_path(struct task *task, const char *path,
                        struct vfs_node_info *info) {
    if (!info) return POSIX_VFS_EINVAL;
    struct kernel_object *node = 0;
    int result = posix_profile_resolve(task, path, &node);
    if (!result) result = node_info(node, info);
    if (node) object_release(node);
    return result;
}

int posix_vfs_mkdir(struct task *task, const char *path, u32 mode) {
    if (mode & ~VFS_MODE_MASK) return POSIX_VFS_EINVAL;
    if (profile_mutation_allowed(task)) return POSIX_VFS_EACCES;
    struct kernel_object *parent = 0;
    char name[VFS_NAME_MAX + 1];
    int result = posix_profile_parent(task, path, &parent, name);
    if (result) return result;
    result = check_parent_mutation(task, parent);
    if (!result) {
        struct kernel_object *existing = vfs_lookup(parent, name);
        if (existing) {
            object_release(existing);
            result = POSIX_VFS_EEXIST;
        } else {
            struct kernel_object *created = vfs_create_mode(
                parent, name, VFS_NODE_DIRECTORY, mode & ~task->umask);
            if (!created) result = POSIX_VFS_ENOSPC;
            else {
                if (vfs_chown(created, task->uid, task->gid)) {
                    vfs_unlink(parent, name);
                    result = POSIX_VFS_EIO;
                }
                object_release(created);
            }
        }
    }
    object_release(parent);
    return result;
}

static int remove_path(struct task *task, const char *path, u32 directory) {
    if (profile_mutation_allowed(task)) return POSIX_VFS_EACCES;
    struct kernel_object *parent = 0;
    char name[VFS_NAME_MAX + 1];
    int result = posix_profile_parent(task, path, &parent, name);
    if (result) return result;
    result = check_parent_mutation(task, parent);
    if (result) {
        object_release(parent);
        return result;
    }
    struct kernel_object *node = vfs_lookup(parent, name);
    struct vfs_node_info info;
    if (!node) result = POSIX_PROFILE_ENOENT;
    else if (node_info(node, &info)) result = POSIX_VFS_EIO;
    else if ((info.type == VFS_NODE_DIRECTORY) != directory)
        result = directory ? POSIX_VFS_ENOTDIR : POSIX_VFS_EISDIR;
    else if (info.readonly) result = POSIX_VFS_EROFS;
    else if (vfs_unlink(parent, name)) result = POSIX_VFS_EBUSY;
    else result = 0;
    if (node) object_release(node);
    object_release(parent);
    return result;
}

int posix_vfs_unlink(struct task *task, const char *path) {
    return remove_path(task, path, 0);
}

int posix_vfs_rmdir(struct task *task, const char *path) {
    return remove_path(task, path, 1);
}

int posix_vfs_link(struct task *task, const char *old_path,
                   const char *new_path) {
    if (profile_mutation_allowed(task)) return POSIX_VFS_EACCES;
    struct kernel_object *node = 0;
    int result = posix_profile_resolve(task, old_path, &node);
    struct vfs_node_info info;
    if (!result) result = node_info(node, &info);
    // Directories never carry a second name, which POSIX reports as EPERM.
    if (!result && info.type == VFS_NODE_DIRECTORY)
        result = POSIX_VFS_EPERM;
    struct kernel_object *parent = 0;
    char name[VFS_NAME_MAX + 1];
    if (!result) result = posix_profile_parent(task, new_path, &parent, name);
    struct vfs_node_info parent_info;
    if (!result) result = node_info(parent, &parent_info);
    // One inode cannot span filesystems, so a pair on different ones is
    // EXDEV rather than a generic refusal; this lands before the parent
    // permission pass so a readonly cross-filesystem target still names
    // the real reason.
    if (!result && parent_info.filesystem != info.filesystem)
        result = POSIX_VFS_EXDEV;
    if (!result) result = check_parent_mutation(task, parent);
    // The VFS alias table caps the name count, and the stat link count is
    // that same number of names.
    if (!result && info.links >= VFS_ALIAS_MAX) result = POSIX_VFS_EMLINK;
    struct kernel_object *existing = 0;
    if (!result) {
        existing = vfs_lookup(parent, name);
        if (existing) result = POSIX_VFS_EEXIST;
    }
    // Every other reject carried its own errno, so a link that still fails
    // here is a same-type cross-mount pair, which is EXDEV too.
    if (!result && vfs_link(node, parent, name)) result = POSIX_VFS_EXDEV;
    if (existing) object_release(existing);
    if (parent) object_release(parent);
    if (node) object_release(node);
    return result;
}

int posix_vfs_rename(struct task *task, const char *old_path,
                     const char *new_path) {
    if (profile_mutation_allowed(task)) return POSIX_VFS_EACCES;
    struct kernel_object *node = 0;
    int result = posix_profile_resolve(task, old_path, &node);
    struct vfs_node_info info;
    if (!result) result = node_info(node, &info);
    struct kernel_object *old_parent = 0;
    char old_name[VFS_NAME_MAX + 1];
    if (!result)
        result = posix_profile_parent(task, old_path, &old_parent, old_name);
    if (!result) result = check_parent_mutation(task, old_parent);
    struct kernel_object *new_parent = 0;
    char new_name[VFS_NAME_MAX + 1];
    if (!result)
        result = posix_profile_parent(task, new_path, &new_parent, new_name);
    // One inode cannot span filesystems, so a pair on different ones is
    // EXDEV rather than a generic refusal; this lands before the new
    // parent permission pass so a readonly cross-filesystem target still
    // names the real reason.
    struct vfs_node_info parent_info;
    if (!result) result = node_info(new_parent, &parent_info);
    if (!result && parent_info.filesystem != info.filesystem)
        result = POSIX_VFS_EXDEV;
    if (!result) result = check_parent_mutation(task, new_parent);
    // The name being replaced, when it exists, decides the errno matrix.
    struct kernel_object *target = 0;
    if (!result) target = vfs_lookup(new_parent, new_name);
    // Renaming a name onto itself is a quiet success, even for a
    // directory with children, which the matrix below would otherwise
    // reject as nonempty.
    if (!result && target == node) {
        if (target) object_release(target);
        if (new_parent) object_release(new_parent);
        if (old_parent) object_release(old_parent);
        if (node) object_release(node);
        return 0;
    }
    struct vfs_node_info target_info;
    if (!result && target) {
        result = node_info(target, &target_info);
        // A file cannot land on a directory, and a directory cannot land
        // on a file; POSIX reports the first as EISDIR and the second as
        // ENOTDIR.
        if (!result && info.type == VFS_NODE_REGULAR &&
            target_info.type == VFS_NODE_DIRECTORY)
            result = POSIX_VFS_EISDIR;
        if (!result && info.type == VFS_NODE_DIRECTORY &&
            target_info.type == VFS_NODE_REGULAR)
            result = POSIX_VFS_ENOTDIR;
        if (!result && info.type == VFS_NODE_DIRECTORY &&
            target_info.type == VFS_NODE_DIRECTORY &&
            target_info.child_count)
            result = POSIX_VFS_ENOTEMPTY;
    }
    // The VFS refuses the rest: mountpoints, directory cycles, and pairs
    // inside two mounts of one volume. The POSIX profile sees none of
    // them, so a failure here reads as EINVAL.
    if (!result && vfs_rename(old_parent, old_name, new_parent, new_name))
        result = POSIX_VFS_EINVAL;
    if (target) object_release(target);
    if (new_parent) object_release(new_parent);
    if (old_parent) object_release(old_parent);
    if (node) object_release(node);
    return result;
}

int posix_vfs_symlink(struct task *task, const char *target,
                      const char *path) {
    if (profile_mutation_allowed(task)) return POSIX_VFS_EACCES;
    // The target is bounded by the path bound, the same rule the node
    // applies when it stores the string inline.
    if (!target || !target[0]) return POSIX_VFS_EINVAL;
    u32 length = 0;
    while (length < VFS_PATH_MAX && target[length]) length++;
    if (length >= VFS_PATH_MAX) return POSIX_PROFILE_ENAMETOOLONG;
    struct kernel_object *parent = 0;
    char name[VFS_NAME_MAX + 1];
    int result = posix_profile_parent(task, path, &parent, name);
    if (result) return result;
    result = check_parent_mutation(task, parent);
    struct kernel_object *existing = 0;
    if (!result) {
        existing = vfs_lookup(parent, name);
        if (existing) result = POSIX_VFS_EEXIST;
        else if (!vfs_symlink(parent, name, target))
            result = POSIX_VFS_ENOSPC;
    }
    if (existing) object_release(existing);
    if (parent) object_release(parent);
    return result;
}

int posix_vfs_readlink(struct task *task, const char *path, char *buffer,
                       u32 size, u32 *length) {
    if (!buffer || !size || !length) return POSIX_VFS_EINVAL;
    // readlink never follows the final name: the link itself is the
    // answer, so the parent walk plus a plain lookup is the whole story.
    struct kernel_object *parent = 0;
    char name[VFS_NAME_MAX + 1];
    int result = posix_profile_parent(task, path, &parent, name);
    struct kernel_object *node = result ? 0 : vfs_lookup(parent, name);
    if (!result && !node) result = POSIX_PROFILE_ENOENT;
    struct vfs_node_info info;
    if (!result) result = node_info(node, &info);
    if (!result && info.type != VFS_NODE_SYMLINK)
        result = POSIX_VFS_EINVAL;
    if (!result && vfs_readlink(node, buffer, size, length))
        result = POSIX_VFS_EIO;
    if (node) object_release(node);
    if (parent) object_release(parent);
    return result;
}

int posix_vfs_lstat_path(struct task *task, const char *path,
                         struct vfs_node_info *info) {
    if (!info) return POSIX_VFS_EINVAL;
    // lstat is the no-follow twin of stat: the final name must resolve to
    // the node itself, symlink or not.
    struct kernel_object *parent = 0;
    char name[VFS_NAME_MAX + 1];
    int result = posix_profile_parent(task, path, &parent, name);
    struct kernel_object *node = result ? 0 : vfs_lookup(parent, name);
    if (!result && !node) result = POSIX_PROFILE_ENOENT;
    if (!result) result = node_info(node, info);
    if (node) object_release(node);
    if (parent) object_release(parent);
    return result;
}

int posix_vfs_truncate_path(struct task *task, const char *path, u32 size) {
    if (size > VFS_FILE_SIZE_MAX) return POSIX_VFS_EFBIG;
    if (profile_mutation_allowed(task)) return POSIX_VFS_EACCES;
    struct kernel_object *node = 0;
    int result = posix_profile_resolve(task, path, &node);
    struct vfs_node_info info;
    if (!result) result = node_info(node, &info);
    if (!result)
        result = check_file_access(task, &info, POSIX_FD_ACCESS_WRITE);
    struct kernel_object *file = !result ? vfs_open(node) : 0;
    if (!result && !file) result = POSIX_VFS_ENFILE;
    if (!result && vfs_truncate(file, size)) result = POSIX_VFS_EIO;
    if (file) object_release(file);
    if (node) object_release(node);
    return result;
}

int posix_vfs_chmod(struct task *task, const char *path, u32 mode) {
    if (mode & ~VFS_MODE_MASK) return POSIX_VFS_EINVAL;
    if (profile_mutation_allowed(task)) return POSIX_VFS_EACCES;
    struct kernel_object *node = 0;
    int result = posix_profile_resolve(task, path, &node);
    struct vfs_node_info info;
    if (!result) result = node_info(node, &info);
    if (!result && info.readonly) result = POSIX_VFS_EROFS;
    // chmod is the owner's call; there is no root override to lean on.
    if (!result && info.uid != task->uid) result = POSIX_VFS_EPERM;
    if (!result && vfs_chmod(node, mode)) result = POSIX_VFS_EIO;
    if (node) object_release(node);
    return result;
}

int posix_vfs_chown(struct task *task, const char *path, i32 uid, i32 gid) {
    if (profile_mutation_allowed(task)) return POSIX_VFS_EACCES;
    struct kernel_object *node = 0;
    int result = posix_profile_resolve(task, path, &node);
    struct vfs_node_info info;
    if (!result) result = node_info(node, &info);
    if (!result && info.readonly) result = POSIX_VFS_EROFS;
    u32 next_uid = uid < 0 ? info.uid : (u32)uid;
    u32 next_gid = gid < 0 ? info.gid : (u32)gid;
    if (!result && task->uid != 0) {
        // A non-root owner cannot hand the file to a different owner and
        // may only regroup within its own single gid.
        if (task->uid != info.uid || next_uid != info.uid ||
            (next_gid != task->gid && next_gid != info.gid))
            result = POSIX_VFS_EPERM;
    }
    if (!result && vfs_chown(node, next_uid, next_gid))
        result = POSIX_VFS_EIO;
    if (node) object_release(node);
    return result;
}
