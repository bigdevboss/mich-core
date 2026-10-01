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
        return adytumfs_super_sync(device, super);
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
    return adytumfs_super_sync(device, super);
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
        if (adytumfs_block_zero(device, super, block)) return -1;
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

// Point one mapped logical block at a different physical block. The covering
// extent splits into up to three pieces and neighbours that stay physically
// adjacent merge back, so the array remains packed from the front. Fails
// closed when the split would need more than the direct slots.
static int adytumfs_extent_reroute(struct adytumfs_inode *inode, u64 logical,
                                   u64 fresh) {
    u32 extents = 0;
    while (extents < ADYTUMFS_DIRECT_EXTENTS && inode->direct[extents].length)
        extents++;

    u64 base = 0;
    for (u32 index = 0; index < extents; index++) {
        u32 length = inode->direct[index].length;
        if (logical < base || logical >= base + length) {
            base += length;
            continue;
        }
        u64 start = inode->direct[index].start_block;
        u64 offset = logical - base;
        struct adytumfs_extent pieces[3];
        u32 count = 0;
        if (offset) {
            pieces[count].start_block = start;
            pieces[count].length = (u32)offset;
            pieces[count].flags = 0;
            count++;
        }
        pieces[count].start_block = fresh;
        pieces[count].length = 1;
        pieces[count].flags = 0;
        count++;
        if (offset + 1 < length) {
            pieces[count].start_block = start + offset + 1;
            pieces[count].length = (u32)(length - offset - 1);
            pieces[count].flags = 0;
            count++;
        }
        if (extents - 1u + count > ADYTUMFS_DIRECT_EXTENTS) return -1;

        struct adytumfs_extent rebuilt[ADYTUMFS_DIRECT_EXTENTS];
        u32 used = 0;
        for (u32 scan = 0; scan < index; scan++)
            rebuilt[used++] = inode->direct[scan];
        for (u32 piece = 0; piece < count; piece++)
            rebuilt[used++] = pieces[piece];
        for (u32 scan = index + 1; scan < extents; scan++)
            rebuilt[used++] = inode->direct[scan];

        // Consecutive slots are logically adjacent, so merging on physical
        // adjacency is representation preserving and undoes the split when
        // the allocator handed out a neighbouring block.
        u32 compact = 0;
        for (u32 scan = 0; scan < used; scan++) {
            if (compact &&
                rebuilt[scan].start_block ==
                    rebuilt[compact - 1].start_block +
                        rebuilt[compact - 1].length) {
                rebuilt[compact - 1].length += rebuilt[scan].length;
                continue;
            }
            rebuilt[compact++] = rebuilt[scan];
        }
        for (u32 scan = 0; scan < ADYTUMFS_DIRECT_EXTENTS; scan++) {
            if (scan < compact) {
                inode->direct[scan] = rebuilt[scan];
            } else {
                inode->direct[scan].start_block = 0;
                inode->direct[scan].length = 0;
                inode->direct[scan].flags = 0;
            }
        }
        return 0;
    }
    return -1;
}

int adytumfs_redirect_stage(struct kernel_object *device,
                            struct adytumfs_superblock *super,
                            struct adytumfs_inode *inode, u64 logical,
                            u64 *old_block, u64 *fresh_block) {
    if (!super || !inode || !old_block || !fresh_block) return -1;
    if (adytumfs_inode_map(inode, logical, old_block)) return -1;
    // The old block is still marked allocated here, so the fresh run can
    // never land on it; nothing else moves until commit.
    return adytumfs_alloc_run(device, super, 1, fresh_block);
}

int adytumfs_redirect_commit(struct kernel_object *device,
                             struct adytumfs_superblock *super,
                             struct adytumfs_inode *inode, u64 inode_num,
                             u64 logical, u64 old_block, u64 fresh_block) {
    if (!super || !inode) return -1;
    struct adytumfs_extent saved[ADYTUMFS_DIRECT_EXTENTS];
    for (u32 index = 0; index < ADYTUMFS_DIRECT_EXTENTS; index++)
        saved[index] = inode->direct[index];
    if (adytumfs_extent_reroute(inode, logical, fresh_block)) {
        if (adytumfs_free_run(device, super, fresh_block, 1)) return -1;
        return -1;
    }
    if (adytumfs_inode_write(device, super, inode_num, inode)) {
        for (u32 index = 0; index < ADYTUMFS_DIRECT_EXTENTS; index++)
            inode->direct[index] = saved[index];
        if (adytumfs_free_run(device, super, fresh_block, 1)) return -1;
        return -1;
    }
    // The old block leaves the pool only after the new mapping is on disk,
    // so the mapping a later boot reads never names a free block.
    return adytumfs_free_run(device, super, old_block, 1);
}

int adytumfs_inode_remap(struct kernel_object *device,
                         struct adytumfs_superblock *super,
                         struct adytumfs_inode *inode, u64 inode_num,
                         u64 logical, const u8 *content) {
    u64 old_block = 0;
    u64 fresh_block = 0;
    if (adytumfs_redirect_stage(device, super, inode, logical, &old_block,
                                &fresh_block))
        return -1;
    if (adytumfs_data_write(device, super, fresh_block, content)) {
        if (adytumfs_free_run(device, super, fresh_block, 1)) return -1;
        return -1;
    }
    return adytumfs_redirect_commit(device, super, inode, inode_num, logical,
                                    old_block, fresh_block);
}
