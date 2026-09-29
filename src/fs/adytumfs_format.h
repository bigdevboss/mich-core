#ifndef ADYTUMFS_FORMAT_H
#define ADYTUMFS_FORMAT_H

#include "types.h"

// AdytumFS on-disk integers are little-endian and are read and written only
// through these helpers, so the big-endian s390x port reads images correctly.
// See mich-notes/adytumfs-format.md for the full layout.

static inline u16 adytumfs_read_le16(const u8 *p) {
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static inline u32 adytumfs_read_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline u64 adytumfs_read_le64(const u8 *p) {
    return (u64)adytumfs_read_le32(p) | ((u64)adytumfs_read_le32(p + 4) << 32);
}

static inline void adytumfs_write_le16(u8 *p, u16 value) {
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
}

static inline void adytumfs_write_le32(u8 *p, u32 value) {
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
    p[2] = (u8)(value >> 16);
    p[3] = (u8)(value >> 24);
}

static inline void adytumfs_write_le64(u8 *p, u64 value) {
    adytumfs_write_le32(p, (u32)value);
    adytumfs_write_le32(p + 4, (u32)(value >> 32));
}

#define ADYTUMFS_BLOCK_SIZE 4096u
#define ADYTUMFS_SECTORS_PER_BLOCK 8u
#define ADYTUMFS_INODE_SIZE 256u
#define ADYTUMFS_INODES_PER_BLOCK 16u
#define ADYTUMFS_NAME_MAX 255u
#define ADYTUMFS_EXTENT_SIZE 16u
#define ADYTUMFS_DIRECT_EXTENTS 10u
#define ADYTUMFS_FORMAT_VERSION 1u
#define ADYTUMFS_ROOT_INODE 1u

#endif
