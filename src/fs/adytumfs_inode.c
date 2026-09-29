#include "adytumfs_format.h"
#include "crc32c.h"

void adytumfs_inode_pack(u8 *slot, const struct adytumfs_inode *inode) {
    for (u32 index = 0; index < ADYTUMFS_INODE_SIZE; index++) slot[index] = 0;
    adytumfs_write_le16(slot + 0, inode->mode);
    adytumfs_write_le16(slot + 2, inode->links);
    adytumfs_write_le32(slot + 4, inode->uid);
    adytumfs_write_le32(slot + 8, inode->gid);
    adytumfs_write_le32(slot + 12, inode->flags);
    adytumfs_write_le64(slot + 16, inode->size);
    adytumfs_write_le64(slot + 24, inode->blocks);
    adytumfs_write_le64(slot + 32, inode->atime);
    adytumfs_write_le64(slot + 40, inode->mtime);
    adytumfs_write_le64(slot + 48, inode->ctime);
    adytumfs_write_le64(slot + 56, inode->generation);
    for (u32 index = 0; index < ADYTUMFS_DIRECT_EXTENTS; index++) {
        u8 *extent = slot + 64 + index * ADYTUMFS_EXTENT_SIZE;
        adytumfs_write_le64(extent + 0, inode->direct[index].start_block);
        adytumfs_write_le32(extent + 8, inode->direct[index].length);
        adytumfs_write_le32(extent + 12, inode->direct[index].flags);
    }
    adytumfs_write_le64(slot + 224, inode->indirect1);
    adytumfs_write_le64(slot + 232, inode->indirect2);
    adytumfs_write_le32(slot + ADYTUMFS_INODE_CHECKSUM_OFFSET,
                        crc32c(slot, ADYTUMFS_INODE_CHECKSUM_OFFSET));
}

int adytumfs_inode_unpack(struct adytumfs_inode *inode, const u8 *slot) {
    u16 mode = adytumfs_read_le16(slot + 0);
    // A free slot has no meaningful checksum, so only in-use inodes are checked.
    if (mode && adytumfs_read_le32(slot + ADYTUMFS_INODE_CHECKSUM_OFFSET) !=
        crc32c(slot, ADYTUMFS_INODE_CHECKSUM_OFFSET))
        return -1;
    inode->mode = mode;
    inode->links = adytumfs_read_le16(slot + 2);
    inode->uid = adytumfs_read_le32(slot + 4);
    inode->gid = adytumfs_read_le32(slot + 8);
    inode->flags = adytumfs_read_le32(slot + 12);
    inode->size = adytumfs_read_le64(slot + 16);
    inode->blocks = adytumfs_read_le64(slot + 24);
    inode->atime = adytumfs_read_le64(slot + 32);
    inode->mtime = adytumfs_read_le64(slot + 40);
    inode->ctime = adytumfs_read_le64(slot + 48);
    inode->generation = adytumfs_read_le64(slot + 56);
    for (u32 index = 0; index < ADYTUMFS_DIRECT_EXTENTS; index++) {
        const u8 *extent = slot + 64 + index * ADYTUMFS_EXTENT_SIZE;
        inode->direct[index].start_block = adytumfs_read_le64(extent + 0);
        inode->direct[index].length = adytumfs_read_le32(extent + 8);
        inode->direct[index].flags = adytumfs_read_le32(extent + 12);
    }
    inode->indirect1 = adytumfs_read_le64(slot + 224);
    inode->indirect2 = adytumfs_read_le64(slot + 232);
    return 0;
}
