#ifndef POSIX_VFS_H
#define POSIX_VFS_H

#include "types.h"
#include "vfs.h"

#define POSIX_OPEN_RDONLY 0u
#define POSIX_OPEN_WRONLY 1u
#define POSIX_OPEN_RDWR 2u
#define POSIX_OPEN_ACCMODE 3u
#define POSIX_OPEN_CREAT 0x40u
#define POSIX_OPEN_TRUNC 0x200u
#define POSIX_OPEN_APPEND 0x400u
#define POSIX_OPEN_CLOEXEC 0x80000u

#define POSIX_VFS_EIO (-5)
#define POSIX_VFS_EBADF (-9)
#define POSIX_VFS_EACCES (-13)
#define POSIX_VFS_EBUSY (-16)
#define POSIX_VFS_EEXIST (-17)
#define POSIX_VFS_ENOTDIR (-20)
#define POSIX_VFS_EISDIR (-21)
#define POSIX_VFS_EINVAL (-22)
#define POSIX_VFS_ENFILE (-23)
#define POSIX_VFS_EMFILE (-24)
#define POSIX_VFS_EFBIG (-27)
#define POSIX_VFS_ENOSPC (-28)
#define POSIX_VFS_EROFS (-30)
#define POSIX_VFS_ERANGE (-34)

struct task;

int posix_vfs_open(struct task *task, const char *path, u32 flags, u32 mode);
int posix_vfs_stat_path(struct task *task, const char *path,
                        struct vfs_node_info *info);
int posix_vfs_mkdir(struct task *task, const char *path, u32 mode);
int posix_vfs_unlink(struct task *task, const char *path);
int posix_vfs_rmdir(struct task *task, const char *path);
int posix_vfs_truncate_path(struct task *task, const char *path, u32 size);

#endif
