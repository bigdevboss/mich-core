#ifndef POSIX_FD_H
#define POSIX_FD_H

#include "types.h"
#include "posix_abi.h"

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
int posix_fd_install_pipe(struct task *task, u32 pipe, u32 end, u32 access);
int posix_fd_pipe_of(struct task *task, int descriptor, u32 *pipe,
                     u32 *end);
int posix_fd_install_socket(struct task *task, struct kernel_object *socket,
                            u32 type);
int posix_fd_socket_of(struct task *task, int descriptor, u32 access,
                       struct kernel_object **socket, u32 *type,
                       u32 *flags, struct posix_sockaddr_in *local,
                       struct posix_sockaddr_in *peer);
int posix_fd_socket_update(struct task *task, int descriptor, u32 flags,
                           const struct posix_sockaddr_in *local,
                           const struct posix_sockaddr_in *peer);
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
int posix_fd_getdents(struct task *task, int descriptor, u8 *buffer,
                      u32 length, u32 *transferred);
int posix_fd_ftruncate(struct task *task, int descriptor, i64 length);
int posix_fd_fsync(struct task *task, int descriptor);
int posix_fd_pread(struct task *task, int descriptor, i64 offset,
                   void *buffer, u32 length, u32 *transferred);
int posix_fd_pwrite(struct task *task, int descriptor, i64 offset,
                    const void *buffer, u32 length, u32 *transferred);
int posix_fd_stat(struct task *task, int descriptor,
                  struct vfs_node_info *info);
int posix_fd_fork(struct task *parent, struct task *child);
void posix_fd_close_cloexec(struct task *task);
int posix_fd_fchmod(struct task *task, int descriptor, u32 mode);
int posix_fd_futimens(struct task *task, int descriptor, i64 atime_sec,
                      i64 atime_nsec, i64 mtime_sec, i64 mtime_nsec);
void posix_fd_close_all(struct task *task);
u32 posix_fd_revoke_object(struct kernel_object *object);
u32 posix_fd_active_count(void);
u32 posix_ofd_active_count(void);

#endif
