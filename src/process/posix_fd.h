#ifndef POSIX_FD_H
#define POSIX_FD_H

#include "types.h"

#define POSIX_FD_MAX 32
#define POSIX_OFD_MAX 32

#define POSIX_FD_ACCESS_READ (1u << 0)
#define POSIX_FD_ACCESS_WRITE (1u << 1)
#define POSIX_FD_APPEND (1u << 2)
#define POSIX_FD_CLOEXEC (1u << 0)

#define POSIX_FD_TABLE_FULL (-2)
#define POSIX_FD_OFD_FULL (-3)

struct task;
struct kernel_object;
struct vfs_node_info;

void posix_fd_init(void);
int posix_fd_install_vfs(struct task *task, struct kernel_object *file,
                         u32 access, u32 status, u32 descriptor_flags);
int posix_fd_validate(struct task *task, int descriptor, u32 access);
int posix_fd_close(struct task *task, int descriptor);
int posix_fd_dup(struct task *task, int descriptor);
int posix_fd_dup2(struct task *task, int descriptor, int replacement);
int posix_fd_get_cloexec(struct task *task, int descriptor, u32 *enabled);
int posix_fd_set_cloexec(struct task *task, int descriptor, u32 enabled);
int posix_fd_read(struct task *task, int descriptor, void *buffer,
                  u32 length, u32 *transferred);
int posix_fd_write(struct task *task, int descriptor, const void *buffer,
                   u32 length, u32 *transferred);
int posix_fd_seek(struct task *task, int descriptor, i64 offset, u32 whence,
                  u32 *position);
int posix_fd_truncate(struct task *task, int descriptor, u32 size);
int posix_fd_stat(struct task *task, int descriptor,
                  struct vfs_node_info *info);
int posix_fd_fork(struct task *parent, struct task *child);
void posix_fd_close_cloexec(struct task *task);
void posix_fd_close_all(struct task *task);
u32 posix_fd_revoke_object(struct kernel_object *object);
u32 posix_fd_active_count(void);
u32 posix_ofd_active_count(void);

#endif
