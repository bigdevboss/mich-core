#ifndef POSIX_PROFILE_H
#define POSIX_PROFILE_H

#include "types.h"
#include "vfs.h"

#define POSIX_PROFILE_EACCES (-13)
#define POSIX_PROFILE_ENOENT (-2)
#define POSIX_PROFILE_ENOTDIR (-20)
#define POSIX_PROFILE_EINVAL (-22)
#define POSIX_PROFILE_EBUSY (-16)
#define POSIX_PROFILE_ENOMEM (-12)
#define POSIX_PROFILE_ENAMETOOLONG (-36)

struct task;
struct kernel_object;

void posix_profile_init(void);
int posix_profile_admit(struct task *task);
int posix_profile_fork(struct task *parent, struct task *child);
void posix_profile_release(struct task *task);
int posix_profile_admitted(const struct task *task);
int posix_profile_vfs_authorized(const struct task *task);
int posix_profile_resolve(struct task *task, const char *path,
                          struct kernel_object **node);
int posix_profile_parent(struct task *task, const char *path,
                         struct kernel_object **parent,
                         char name[VFS_NAME_MAX]);
int posix_profile_chdir(struct task *task, const char *path);
int posix_profile_getcwd(struct task *task, char path[VFS_PATH_MAX]);

#endif
