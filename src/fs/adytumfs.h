#ifndef ADYTUMFS_H
#define ADYTUMFS_H

#include "types.h"
#include "object.h"

#define ADYTUMFS_INODE_MAX 16
#define ADYTUMFS_FILE_PAGES 16
#define ADYTUMFS_FILE_SIZE_MAX (ADYTUMFS_FILE_PAGES * 4096u)

int adytumfs_format(struct kernel_object *device);
int adytumfs_attach(u32 mount, struct kernel_object *device);
void adytumfs_detach(u32 mount);
u32 adytumfs_inode_count(u32 mount);
int adytumfs_inode_get(u32 mount, u32 inode, u32 *used, u32 *type, u32 *size,
                      u32 *parent, u32 *mode, char *name, u64 *generation);
int adytumfs_inode_create(u32 mount, const char *name, u32 parent, u32 type,
                         u32 mode, u32 *inode, u64 *generation);
int adytumfs_inode_remove(u32 mount, u32 inode);
int adytumfs_truncate(u32 mount, u32 inode, u64 generation, u32 size,
                     u32 *new_size);
int adytumfs_pages_attach(u32 mount, u32 inode, u64 generation,
                         struct kernel_object *pages);
int adytumfs_pages_fault(u32 mount, u32 inode, u64 generation, u32 page);
int adytumfs_pages_dirty(u32 mount, u32 inode, u64 generation, u32 page,
                        u32 size);
int adytumfs_pages_sync(u32 mount, u32 inode, u64 generation);
void adytumfs_pages_detach(u32 mount, u32 inode, u64 generation);

#endif
