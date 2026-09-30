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

// One staging block for the inode table; the single-CPU filesystem reads,
// modifies, and writes it in place, and the block cache keeps repeats cheap.
static u8 adytumfs_inode_scratch[ADYTUMFS_BLOCK_SIZE];

int adytumfs_inode_read(struct kernel_object *device,
                        const struct adytumfs_superblock *super,
                        u64 inode_num, struct adytumfs_inode *out) {
    if (!super || !out || inode_num == 0 || inode_num >= super->inode_count)
        return -1;
    u64 block = super->inode_table_start + inode_num / ADYTUMFS_INODES_PER_BLOCK;
    u32 slot = (u32)(inode_num % ADYTUMFS_INODES_PER_BLOCK) * ADYTUMFS_INODE_SIZE;
    if (adytumfs_block_read(device, block, adytumfs_inode_scratch)) return -1;
    return adytumfs_inode_unpack(out, adytumfs_inode_scratch + slot);
}

int adytumfs_inode_write(struct kernel_object *device,
                         const struct adytumfs_superblock *super,
                         u64 inode_num, const struct adytumfs_inode *in) {
    if (!super || !in || inode_num == 0 || inode_num >= super->inode_count)
        return -1;
    u64 block = super->inode_table_start + inode_num / ADYTUMFS_INODES_PER_BLOCK;
    u32 slot = (u32)(inode_num % ADYTUMFS_INODES_PER_BLOCK) * ADYTUMFS_INODE_SIZE;
    // Read-modify-write: the other fifteen inodes in this block must survive.
    if (adytumfs_block_read(device, block, adytumfs_inode_scratch)) return -1;
    adytumfs_inode_pack(adytumfs_inode_scratch + slot, in);
    return adytumfs_block_write(device, block, adytumfs_inode_scratch);
}

int adytumfs_inode_alloc(struct kernel_object *device,
                         struct adytumfs_superblock *super,
                         u16 mode, u64 *inode_num) {
    if (!super || !inode_num || mode == 0) return -1;
    for (u64 candidate = 1; candidate < super->inode_count; candidate++) {
        struct adytumfs_inode inode;
        if (adytumfs_inode_read(device, super, candidate, &inode)) return -1;
        if (inode.mode) continue;
        // Reusing a slot bumps its generation so a handle to the old inode that
        // outlives it fails the generation check rather than aliasing the new one.
        struct adytumfs_inode fresh = {0};
        fresh.mode = mode;
        fresh.links = 1;
        fresh.generation = inode.generation + 1;
        if (adytumfs_inode_write(device, super, candidate, &fresh)) return -1;
        super->free_inodes--;
        *inode_num = candidate;
        return 0;
    }
    return -1;
}

int adytumfs_inode_free(struct kernel_object *device,
                        struct adytumfs_superblock *super, u64 inode_num) {
    if (!super || inode_num == 0 || inode_num >= super->inode_count) return -1;
    struct adytumfs_inode inode;
    if (adytumfs_inode_read(device, super, inode_num, &inode)) return -1;
    if (inode.mode == 0) return -1;
    inode.mode = 0;
    if (adytumfs_inode_write(device, super, inode_num, &inode)) return -1;
    super->free_inodes++;
    return 0;
}

int adytumfs_inode_map(const struct adytumfs_inode *inode, u64 logical_block,
                       u64 *physical_block) {
    if (!inode || !physical_block) return -1;
    u64 base = 0;
    // Extents are packed from the front; a zero-length extent ends the list.
    for (u32 index = 0; index < ADYTUMFS_DIRECT_EXTENTS; index++) {
        u32 length = inode->direct[index].length;
        if (length == 0) break;
        if (logical_block < base + length) {
            *physical_block =
                inode->direct[index].start_block + (logical_block - base);
            return 0;
        }
        base += length;
    }
    return -1;
}

int adytumfs_inode_grow(struct kernel_object *device,
                        struct adytumfs_superblock *super,
                        struct adytumfs_inode *inode, u64 new_blocks) {
    if (!super || !inode) return -1;
    if (new_blocks <= inode->blocks) return 0;
    u64 add = new_blocks - inode->blocks;
    u64 start;
    if (adytumfs_alloc_run(device, super, add, &start)) return -1;

    u32 last = 0;
    int have = 0;
    for (u32 index = 0; index < ADYTUMFS_DIRECT_EXTENTS; index++) {
        if (inode->direct[index].length == 0) break;
        last = index;
        have = 1;
    }
    if (have && inode->direct[last].start_block + inode->direct[last].length ==
        start) {
        inode->direct[last].length += (u32)add;
    } else {
        u32 slot = have ? last + 1 : 0;
        if (slot >= ADYTUMFS_DIRECT_EXTENTS) {
            // No direct slot left; indirect blocks are not implemented yet, so
            // give the run back rather than lose track of it.
            adytumfs_free_run(device, super, start, add);
            return -1;
        }
        inode->direct[slot].start_block = start;
        inode->direct[slot].length = (u32)add;
        inode->direct[slot].flags = 0;
    }
    inode->blocks = new_blocks;
    // A reallocated block still holds the previous file's bytes. Reads clamp
    // to size, but a mapping exposes whole pages and a write that skipped
    // pages must read back as zeroes, so every freshly allocated block is
    // zeroed here. The zeroing is direct I/O on purpose: the pages path moves
    // file data with direct transfers, and a cached zero would be flushed
    // over the real bytes later.
    for (u64 block = start; block < start + add; block++)
        if (adytumfs_block_zero(device, block)) return -1;
    return 0;
}

int adytumfs_inode_truncate(struct kernel_object *device,
                            struct adytumfs_superblock *super,
                            struct adytumfs_inode *inode, u64 new_blocks) {
    if (!super || !inode) return -1;
    if (new_blocks >= inode->blocks) return 0;
    u64 base = 0;
    for (u32 index = 0; index < ADYTUMFS_DIRECT_EXTENTS; index++) {
        u32 length = inode->direct[index].length;
        if (length == 0) break;
        u64 end = base + length;
        if (base >= new_blocks) {
            if (adytumfs_free_run(device, super, inode->direct[index].start_block,
                                  length))
                return -1;
            inode->direct[index].start_block = 0;
            inode->direct[index].length = 0;
            inode->direct[index].flags = 0;
        } else if (end > new_blocks) {
            u64 keep = new_blocks - base;
            if (adytumfs_free_run(device, super,
                                  inode->direct[index].start_block + keep,
                                  length - keep))
                return -1;
            inode->direct[index].length = (u32)keep;
        }
        base = end;
    }
    inode->blocks = new_blocks;
    return 0;
}
