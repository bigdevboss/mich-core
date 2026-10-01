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
// Stamp inode times through the open window. MTIME and CTIME follow data
// writes and metadata edits; ATIME applies the relatime-lite rule and only
// refreshes while atime trails mtime, so a steady-state read stages nothing.
#define ADYTUMFS_TOUCH_ATIME 1u
#define ADYTUMFS_TOUCH_MTIME 2u
#define ADYTUMFS_TOUCH_CTIME 4u
int adytumfs_touch(u32 mount, u32 inode, u64 generation, u32 flags);
// The live ownership, link count, and time picture of one inode, read
// through the window staging.
int adytumfs_inode_meta(u32 mount, u32 inode, u64 generation, u16 *links,
                        u32 *uid, u32 *gid, u64 *atime, u64 *mtime,
                        u64 *ctime);
// One used on-disk directory entry per call, mirroring vfs_read_dir: the
// cursor is a byte offset in the directory blocks and the returned inode is
// the on-disk number.
int adytumfs_dir_read(u32 mount, u32 inode, u64 generation, u64 *cursor,
                      char *name, u32 *name_len, u64 *entry_inode,
                      u32 *type);
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
