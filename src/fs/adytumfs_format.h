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

// The superblock lives in block 0 (a backup copy in the last block). Its
// checksum is the last field and covers every byte before it, so it never has
// to hash itself.
#define ADYTUMFS_SIGNATURE "AdytumFS"
#define ADYTUMFS_SIGNATURE_SIZE 8u
#define ADYTUMFS_SUPER_CHECKSUM_OFFSET 4092u

struct adytumfs_superblock {
    u32 format_version;
    u32 block_size;
    u64 total_blocks;
    u64 block_bitmap_start;
    u64 block_bitmap_blocks;
    u64 inode_table_start;
    u64 inode_table_blocks;
    u64 inode_count;
    u64 root_inode;
    u64 data_start;
    u64 free_blocks;
    u64 free_inodes;
    u64 generation;
    u64 feature_compat;
    u64 feature_incompat;
    u64 feature_ro_compat;
    u64 data_checksum_region;
};

// Serialise a superblock into a 4 KiB block (little-endian, checksum computed).
void adytumfs_super_pack(u8 *block, const struct adytumfs_superblock *super);
// Parse a 4 KiB block: verifies signature, version, block size, and checksum.
int adytumfs_super_unpack(struct adytumfs_superblock *super, const u8 *block);
// Check that the region layout is internally consistent and fits device_blocks.
int adytumfs_super_valid(const struct adytumfs_superblock *super,
                         u64 device_blocks);

// Inode type bits, laid over the low permission bits inside mode (POSIX style).
#define ADYTUMFS_MODE_DIR 0x4000u
#define ADYTUMFS_MODE_REG 0x8000u
#define ADYTUMFS_INODE_CHECKSUM_OFFSET 252u

struct adytumfs_extent {
    u64 start_block;
    u32 length;
    u32 flags;
};

struct adytumfs_inode {
    u16 mode;
    u16 links;
    u32 uid;
    u32 gid;
    u32 flags;
    u64 size;
    u64 blocks;
    u64 atime;
    u64 mtime;
    u64 ctime;
    u64 generation;
    struct adytumfs_extent direct[ADYTUMFS_DIRECT_EXTENTS];
    u64 indirect1;
    u64 indirect2;
};

// Serialise an inode into its 256-byte table slot (little-endian, checksum set).
void adytumfs_inode_pack(u8 *slot, const struct adytumfs_inode *inode);
// Parse an inode slot. A free slot (mode 0) carries no checksum and always
// parses; an in-use inode with a bad checksum is rejected.
int adytumfs_inode_unpack(struct adytumfs_inode *inode, const u8 *slot);

struct kernel_object;

// Read or write one 4 KiB filesystem block (eight device sectors) by block
// number, through the block cache.
int adytumfs_block_read(struct kernel_object *device, u64 block, u8 *buffer);
int adytumfs_block_write(struct kernel_object *device, u64 block,
                         const u8 *buffer);
// Format a device: lay out the regions and write the superblock (and its
// backup), the block bitmap, and an empty root directory.
int adytumfs_make(struct kernel_object *device);

#endif
