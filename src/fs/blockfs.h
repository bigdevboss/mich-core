#ifndef BLOCKFS_H
#define BLOCKFS_H

#include "types.h"
#include "object.h"

#define BLOCKFS_MAGIC 0x3146484Du
#define BLOCKFS_INODE_MAX 16
#define BLOCKFS_FILE_SECTORS 8
#define BLOCKFS_FILE_SIZE_MAX (BLOCKFS_FILE_SECTORS * BLOCK_SECTOR_SIZE)

int blockfs_format(struct kernel_object *device);
int blockfs_attach(u32 mount, struct kernel_object *device);
void blockfs_detach(u32 mount);
u32 blockfs_inode_count(u32 mount);
int blockfs_inode_get(u32 mount, u32 inode, u32 *used, u32 *type, u32 *size,
                      u32 *parent, u32 *mode, char *name);
int blockfs_inode_create(u32 mount, const char *name, u32 parent, u32 type,
                         u32 mode, u32 *inode);
int blockfs_inode_remove(u32 mount, u32 inode);
int blockfs_read(u32 mount, u32 inode, u32 offset, void *buffer,
                 u32 length, u32 *transferred);
int blockfs_write(u32 mount, u32 inode, u32 offset, const void *buffer,
                  u32 length, u32 *transferred, u32 *size);
int blockfs_truncate(u32 mount, u32 inode, u32 size, u32 *new_size);

#endif
