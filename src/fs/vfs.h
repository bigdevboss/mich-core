#ifndef VFS_H
#define VFS_H

#include "types.h"
#include "object.h"

#define VFS_NODE_MAX 64
#define VFS_FILE_MAX 32
#define VFS_MOUNT_MAX 8
#define VFS_NAME_MAX 32
#define VFS_FILE_SIZE_MAX 4096
#define VFS_PATH_MAX 256
#define VFS_PATH_COMPONENT_MAX 32

#define VFS_NODE_REGULAR 1
#define VFS_NODE_DIRECTORY 2

/* Special nodes generate their content at read time instead of storing
   it, so size and offset carry no meaning for them. */
#define VFS_SPECIAL_NONE 0u
#define VFS_SPECIAL_URANDOM 1u

#define VFS_FILESYSTEM_RAMFS 1
#define VFS_FILESYSTEM_BOOTFS 2
#define VFS_FILESYSTEM_BLOCKFS 3
#define VFS_BOOTFS_ENTRY_MAX 16
#define VFS_BOOTFS_FILE_SIZE_MAX 0x100000

#define VFS_MODE_MASK 0777u
#define VFS_MODE_REGULAR_DEFAULT 0666u
#define VFS_MODE_DIRECTORY_DEFAULT 0777u
#define VFS_MODE_REGULAR_READONLY 0444u
#define VFS_MODE_DIRECTORY_READONLY 0555u

struct vfs_node_info {
    u32 type;
    u32 generation;
    u32 size;
    u32 child_count;
    u32 linked;
    u32 filesystem;
    u32 readonly;
    u32 mode;
    char name[VFS_NAME_MAX];
};

struct vfs_bootfs_entry {
    const char *name;
    const void *data;
    u32 size;
};

void vfs_init(void);
struct kernel_object *vfs_root(void);
struct kernel_object *vfs_mount_root(void);
int vfs_mount_bootfs(struct kernel_object *directory,
                     const struct vfs_bootfs_entry *entries, u32 count);
int vfs_mount_blockfs(struct kernel_object *directory,
                      struct kernel_object *device);
int vfs_unmount(struct kernel_object *directory);
struct kernel_object *vfs_create(struct kernel_object *directory,
                                 const char *name, u32 type);
struct kernel_object *vfs_create_urandom(struct kernel_object *directory);
struct kernel_object *vfs_create_mode(struct kernel_object *directory,
                                      const char *name, u32 type, u32 mode);
struct kernel_object *vfs_lookup(struct kernel_object *directory,
                                 const char *name);
struct kernel_object *vfs_resolve(struct kernel_object *start,
                                  const char *path);
struct kernel_object *vfs_create_path(struct kernel_object *start,
                                      const char *path, u32 type);
int vfs_unlink_path(struct kernel_object *start, const char *path);
int vfs_unlink(struct kernel_object *directory, const char *name);
struct kernel_object *vfs_open(struct kernel_object *node);
int vfs_image(struct kernel_object *node, const u8 **data, u32 *size);
int vfs_read(struct kernel_object *file, u32 offset,
             void *buffer, u32 length, u32 *transferred);
int vfs_write(struct kernel_object *file, u32 offset,
              const void *buffer, u32 length, u32 *transferred);
int vfs_append(struct kernel_object *file, const void *buffer, u32 length,
               u32 *transferred, u32 *position);
int vfs_truncate(struct kernel_object *file, u32 size);
int vfs_stat(struct kernel_object *object, struct vfs_node_info *info);
u32 vfs_node_active_count(void);
u32 vfs_file_active_count(void);
u32 vfs_mount_active_count(void);

#endif
