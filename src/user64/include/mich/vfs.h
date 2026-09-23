#ifndef MICH64_USER_VFS_H
#define MICH64_USER_VFS_H

#include <vfs_abi.h>

#define MICH_VFS_NAME_MAX VFS_NAME_MAX
#define MICH_VFS_IO_MAX VFS_IO_MAX
#define MICH_VFS_PATH_MAX VFS_PATH_MAX
#define MICH_VFS_NODE_REGULAR VFS_NODE_REGULAR
#define MICH_VFS_NODE_DIRECTORY VFS_NODE_DIRECTORY
#define MICH_VFS_FILESYSTEM_RAMFS VFS_FILESYSTEM_RAMFS
#define MICH_VFS_FILESYSTEM_BOOTFS VFS_FILESYSTEM_BOOTFS
#define MICH_VFS_RIGHT_READ (1u << 0)
#define MICH_VFS_RIGHT_WRITE (1u << 1)

#define mich_vfs_name_request vfs_name_request
#define mich_vfs_open_request vfs_open_request
#define mich_vfs_io_request vfs_io_request
#define mich_vfs_stat_request vfs_stat_request
#define mich_vfs_truncate_request vfs_truncate_request
#define mich_vfs_path_request vfs_path_request
#define mich_vfs_node_info vfs_node_info

int mich_vfs_root(void);
int mich_vfs_create(struct mich_vfs_name_request *request);
int mich_vfs_lookup(struct mich_vfs_name_request *request);
int mich_vfs_open(struct mich_vfs_open_request *request);
int mich_vfs_read(struct mich_vfs_io_request *request);
int mich_file_map(unsigned int file_handle,
                  unsigned long long virtual_address);
int mich_vfs_write(struct mich_vfs_io_request *request);
int mich_vfs_truncate(struct mich_vfs_truncate_request *request);
int mich_vfs_stat(struct mich_vfs_stat_request *request);
int mich_vfs_unlink(struct mich_vfs_name_request *request);
int mich_vfs_resolve(struct mich_vfs_path_request *request);
int mich_vfs_create_path(struct mich_vfs_path_request *request);
int mich_vfs_unlink_path(struct mich_vfs_path_request *request);

#endif
