#ifndef VFS_ABI_H
#define VFS_ABI_H

#include "types.h"
#include "vfs.h"

#define VFS_IO_MAX 512

struct vfs_name_request {
    u32 directory_handle;
    u32 type;
    u32 node_handle;
    u32 reserved;
    char name[VFS_NAME_MAX];
};

struct vfs_open_request {
    u32 node_handle;
    u32 rights;
    u32 file_handle;
    u32 reserved;
};

struct vfs_io_request {
    u32 file_handle;
    u32 offset;
    u32 length;
    u32 transferred;
    u8 data[VFS_IO_MAX];
};

struct vfs_stat_request {
    u32 handle;
    u32 reserved;
    struct vfs_node_info info;
};

struct vfs_truncate_request {
    u32 file_handle;
    u32 size;
};

struct vfs_path_request {
    u32 start_handle;
    u32 type;
    u32 node_handle;
    u32 reserved;
    char path[VFS_PATH_MAX];
};

#endif
