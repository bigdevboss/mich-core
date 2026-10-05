#ifndef POSIX_VFS_H
#define POSIX_VFS_H

#include "types.h"
#include "vfs.h"
#include "posix_abi.h"

#define POSIX_OPEN_RDONLY 0u
#define POSIX_OPEN_WRONLY 1u
#define POSIX_OPEN_RDWR 2u
// access asks whether the calling uid may reach a path: F_OK is a bare
// existence probe and the rest are the classic permission questions.
#define POSIX_ACCESS_F_OK 0u
#define POSIX_ACCESS_R_OK 4u
#define POSIX_ACCESS_W_OK 2u
#define POSIX_ACCESS_X_OK 1u
#define POSIX_OPEN_ACCMODE 3u
#define POSIX_OPEN_CREAT 0x40u
#define POSIX_OPEN_TRUNC 0x200u
#define POSIX_OPEN_APPEND 0x400u
#define POSIX_OPEN_CLOEXEC 0x80000u

#define POSIX_VFS_EPERM (-1)
#define POSIX_VFS_EIO (-5)
#define POSIX_VFS_EBADF (-9)
#define POSIX_VFS_EACCES (-13)
#define POSIX_VFS_EBUSY (-16)
#define POSIX_VFS_EEXIST (-17)
#define POSIX_VFS_EXDEV (-18)
#define POSIX_VFS_ENOTDIR (-20)
#define POSIX_VFS_EISDIR (-21)
#define POSIX_VFS_EINVAL (-22)
#define POSIX_VFS_ENFILE (-23)
#define POSIX_VFS_EMFILE (-24)
#define POSIX_VFS_EFBIG (-27)
#define POSIX_VFS_ENOSPC (-28)
#define POSIX_VFS_EMLINK (-31)
#define POSIX_VFS_EROFS (-30)
#define POSIX_VFS_ERANGE (-34)
#define POSIX_VFS_EPIPE (-32)
#define POSIX_VFS_EDEADLK (-35)
#define POSIX_VFS_ENOTEMPTY (-39)
#define POSIX_VFS_ELOOP (-40)

struct task;

int posix_vfs_open(struct task *task, const char *path, u32 flags, u32 mode);
int posix_vfs_stat_path(struct task *task, const char *path,
                        struct vfs_node_info *info);
int posix_vfs_mkdir(struct task *task, const char *path, u32 mode);
int posix_vfs_unlink(struct task *task, const char *path);
int posix_vfs_link(struct task *task, const char *old_path,
                   const char *new_path);
int posix_vfs_rename(struct task *task, const char *old_path,
                     const char *new_path);
int posix_vfs_symlink(struct task *task, const char *target,
                      const char *path);
int posix_vfs_readlink(struct task *task, const char *path, char *buffer,
                       u32 size, u32 *length);
int posix_vfs_lstat_path(struct task *task, const char *path,
                         struct vfs_node_info *info);
int posix_vfs_rmdir(struct task *task, const char *path);
int posix_vfs_truncate_path(struct task *task, const char *path, u32 size);
int posix_vfs_chmod(struct task *task, const char *path, u32 mode);
int posix_vfs_access(struct task *task, const char *path, u32 mode);
int posix_vfs_chown(struct task *task, const char *path, i32 uid, i32 gid);
int posix_vfs_utimensat(struct task *task, const char *path, i64 atime_sec,
                        i64 atime_nsec, i64 mtime_sec, i64 mtime_nsec);

// The shared half of utimensat and futimens: check both tv_nsec spellings,
// turn NOW into the sampled clock, and say which permission question the
// caller still has to answer. The flags name the fields the vfs call sets,
// and both OMIT leaves no flags at all. 0 on success, -EINVAL on a
// nanosecond field outside [0, 999999999].
#define POSIX_UTIMES_ASK_NONE 0u
#define POSIX_UTIMES_ASK_OWNER 1u
#define POSIX_UTIMES_ASK_OWNER_OR_WRITE 2u
int posix_vfs_utimens_prepare(i64 atime_sec, i64 atime_nsec, i64 mtime_sec,
                              i64 mtime_nsec, u32 *flags, u64 *atime,
                              u32 *atime_nsec_out, u64 *mtime,
                              u32 *mtime_nsec_out, u32 *permission);

#endif
