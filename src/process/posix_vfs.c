#include "posix_vfs.h"
#include "posix_profile.h"
#include "posix_fd.h"
#include "object.h"

static int profile_mutation_allowed(struct task *task) {
    return posix_profile_vfs_authorized(task) ? 0 : POSIX_VFS_EACCES;
}

static int node_info(struct kernel_object *node, struct vfs_node_info *info) {
    return node && info && !vfs_stat(node, info) ? 0 : POSIX_VFS_EIO;
}

static int check_parent_mutation(struct kernel_object *parent) {
    struct vfs_node_info info;
    int result = node_info(parent, &info);
    if (result) return result;
    if (info.type != VFS_NODE_DIRECTORY) return POSIX_VFS_ENOTDIR;
    if (info.readonly) return POSIX_VFS_EROFS;
    if ((info.mode & 0300u) != 0300u) return POSIX_VFS_EACCES;
    return 0;
}

static int check_file_access(const struct vfs_node_info *info, u32 access) {
    if (info->type == VFS_NODE_DIRECTORY) return POSIX_VFS_EISDIR;
    if (info->type != VFS_NODE_REGULAR) return POSIX_VFS_EIO;
    if ((access & POSIX_FD_ACCESS_READ) && !(info->mode & 0400u))
        return POSIX_VFS_EACCES;
    if (access & POSIX_FD_ACCESS_WRITE) {
        if (info->readonly) return POSIX_VFS_EROFS;
        if (!(info->mode & 0200u)) return POSIX_VFS_EACCES;
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
    char name[VFS_NAME_MAX];
    int created = 0;
    result = posix_profile_resolve(task, path, &node);
    if (result == POSIX_PROFILE_ENOENT && (flags & POSIX_OPEN_CREAT)) {
        result = posix_profile_parent(task, path, &parent, name);
        if (result) return result;
        result = check_parent_mutation(parent);
        if (result) {
            object_release(parent);
            return result;
        }
        node = vfs_create_mode(parent, name, VFS_NODE_REGULAR, mode);
        if (!node) {
            node = vfs_lookup(parent, name);
            if (!node) {
                object_release(parent);
                return POSIX_VFS_ENOSPC;
            }
        } else {
            created = 1;
        }
    } else if (result) {
        return result;
    }
    struct vfs_node_info info;
    result = node_info(node, &info);
    if (!result) result = check_file_access(&info, access);
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
    char name[VFS_NAME_MAX];
    int result = posix_profile_parent(task, path, &parent, name);
    if (result) return result;
    result = check_parent_mutation(parent);
    if (!result) {
        struct kernel_object *existing = vfs_lookup(parent, name);
        if (existing) {
            object_release(existing);
            result = POSIX_VFS_EEXIST;
        } else {
            struct kernel_object *created = vfs_create_mode(
                parent, name, VFS_NODE_DIRECTORY, mode);
            if (!created) result = POSIX_VFS_ENOSPC;
            else object_release(created);
        }
    }
    object_release(parent);
    return result;
}

static int remove_path(struct task *task, const char *path, u32 directory) {
    if (profile_mutation_allowed(task)) return POSIX_VFS_EACCES;
    struct kernel_object *parent = 0;
    char name[VFS_NAME_MAX];
    int result = posix_profile_parent(task, path, &parent, name);
    if (result) return result;
    result = check_parent_mutation(parent);
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

int posix_vfs_truncate_path(struct task *task, const char *path, u32 size) {
    if (size > VFS_FILE_SIZE_MAX) return POSIX_VFS_EFBIG;
    if (profile_mutation_allowed(task)) return POSIX_VFS_EACCES;
    struct kernel_object *node = 0;
    int result = posix_profile_resolve(task, path, &node);
    struct vfs_node_info info;
    if (!result) result = node_info(node, &info);
    if (!result) result = check_file_access(&info, POSIX_FD_ACCESS_WRITE);
    struct kernel_object *file = !result ? vfs_open(node) : 0;
    if (!result && !file) result = POSIX_VFS_ENFILE;
    if (!result && vfs_truncate(file, size)) result = POSIX_VFS_EIO;
    if (file) object_release(file);
    if (node) object_release(node);
    return result;
}
