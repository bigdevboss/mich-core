#include "posix_fd.h"
#include "task.h"
#include "object.h"
#include "vfs.h"
#include "posix_vfs.h"
#include "posix_profile.h"
#include "posix_abi.h"
#include "spinlock.h"

struct posix_ofd {
    struct kernel_object *file;
    struct spinlock lock;
    u32 references;
    u32 access;
    u32 status;
    u32 offset;
    u32 active;
};

struct posix_fd_entry {
    struct posix_ofd *ofd;
    u32 flags;
};

static struct posix_ofd ofds[POSIX_OFD_MAX];
static struct posix_fd_entry tables[MAX_TASKS][POSIX_FD_MAX];
static struct spinlock posix_fd_lock = SPINLOCK_INIT;

static int task_slot(const struct task *task) {
    uptr_t address = (uptr_t)task;
    uptr_t base = (uptr_t)task_pool;
    if (!task || address < base ||
        address >= base + sizeof(task_pool) ||
        (address - base) % sizeof(struct task))
        return -1;
    return (int)((address - base) / sizeof(struct task));
}

static int live_task_slot(const struct task *task) {
    int slot = task_slot(task);
    if (slot < 0 || slot >= task_pool_count ||
        task_pool[slot].state == TASK_FREE ||
        task_pool[slot].state == TASK_ZOMBIE)
        return -1;
    return slot;
}

static struct posix_ofd *descriptor_for_locked(u32 slot, int descriptor) {
    if (descriptor < 0 || descriptor >= POSIX_FD_MAX) return 0;
    struct posix_ofd *ofd = tables[slot][descriptor].ofd;
    if (!ofd || !ofd->active || !ofd->references) return 0;
    return ofd;
}

static int ofd_retain_locked(struct posix_ofd *ofd) {
    if (!ofd || !ofd->active || !ofd->references ||
        ofd->references == 0xFFFFFFFFu)
        return -1;
    ofd->references++;
    return 0;
}

static struct kernel_object *ofd_release_locked(struct posix_ofd *ofd) {
    if (!ofd || !ofd->active || !ofd->references) return 0;
    ofd->references--;
    if (ofd->references) return 0;
    struct kernel_object *file = ofd->file;
    ofd->file = 0;
    ofd->access = 0;
    ofd->status = 0;
    ofd->offset = 0;
    ofd->active = 0;
    ofd->lock.ticket = 0;
    ofd->lock.served = 0;
    return file;
}

static struct kernel_object *detach_descriptor_locked(u32 slot,
                                                       u32 descriptor) {
    struct posix_ofd *ofd = tables[slot][descriptor].ofd;
    tables[slot][descriptor].ofd = 0;
    tables[slot][descriptor].flags = 0;
    return ofd_release_locked(ofd);
}

static void release_files(struct kernel_object **files, u32 count) {
    for (u32 index = 0; index < count; index++)
        if (files[index]) object_release(files[index]);
}

static void release_ofd(struct posix_ofd *ofd) {
    spin_lock(&posix_fd_lock);
    struct kernel_object *file = ofd_release_locked(ofd);
    spin_unlock(&posix_fd_lock);
    if (file) object_release(file);
}

static struct posix_ofd *retain_descriptor(struct task *task, int descriptor,
                                           u32 required_access) {
    int slot = live_task_slot(task);
    if (slot < 0) return 0;
    spin_lock(&posix_fd_lock);
    struct posix_ofd *ofd = descriptor_for_locked((u32)slot, descriptor);
    if (!ofd || (required_access & ~ofd->access) || ofd_retain_locked(ofd))
        ofd = 0;
    spin_unlock(&posix_fd_lock);
    return ofd;
}

void posix_fd_init(void) {
    posix_fd_lock.ticket = 0;
    posix_fd_lock.served = 0;
    for (u32 index = 0; index < POSIX_OFD_MAX; index++) {
        ofds[index].file = 0;
        ofds[index].lock.ticket = 0;
        ofds[index].lock.served = 0;
        ofds[index].references = 0;
        ofds[index].access = 0;
        ofds[index].status = 0;
        ofds[index].offset = 0;
        ofds[index].active = 0;
    }
    for (u32 task = 0; task < MAX_TASKS; task++)
        for (u32 descriptor = 0; descriptor < POSIX_FD_MAX; descriptor++) {
            tables[task][descriptor].ofd = 0;
            tables[task][descriptor].flags = 0;
        }
}

int posix_fd_install_vfs(struct task *task, struct kernel_object *file,
                         u32 access, u32 status, u32 descriptor_flags) {
    int slot = live_task_slot(task);
    if (slot < 0 || !file || file->type != KOBJECT_FILE ||
        !(access & (POSIX_FD_ACCESS_READ | POSIX_FD_ACCESS_WRITE)) ||
        (access & ~(POSIX_FD_ACCESS_READ | POSIX_FD_ACCESS_WRITE)) ||
        (status & ~POSIX_FD_APPEND) ||
        ((status & POSIX_FD_APPEND) && !(access & POSIX_FD_ACCESS_WRITE)) ||
        (descriptor_flags & ~POSIX_FD_CLOEXEC))
        return -1;
    struct vfs_node_info info;
    if (vfs_stat(file, &info) || object_retain(file)) return -1;

    spin_lock(&posix_fd_lock);
    u32 descriptor = POSIX_FD_MAX;
    u32 index = POSIX_OFD_MAX;
    for (u32 current = 0; current < POSIX_FD_MAX; current++)
        if (!tables[slot][current].ofd) {
            descriptor = current;
            break;
        }
    for (u32 current = 0; current < POSIX_OFD_MAX; current++)
        if (!ofds[current].active) {
            index = current;
            break;
        }
    if (descriptor == POSIX_FD_MAX || index == POSIX_OFD_MAX) {
        int result = descriptor == POSIX_FD_MAX ? POSIX_FD_TABLE_FULL :
            POSIX_FD_OFD_FULL;
        spin_unlock(&posix_fd_lock);
        object_release(file);
        return result;
    }
    struct posix_ofd *ofd = &ofds[index];
    ofd->file = file;
    ofd->lock.ticket = 0;
    ofd->lock.served = 0;
    ofd->references = 1;
    ofd->access = access;
    ofd->status = status;
    ofd->offset = 0;
    ofd->active = 1;
    tables[slot][descriptor].ofd = ofd;
    tables[slot][descriptor].flags = descriptor_flags;
    spin_unlock(&posix_fd_lock);
    return (int)descriptor;
}

int posix_fd_validate(struct task *task, int descriptor, u32 access) {
    int slot = live_task_slot(task);
    if (slot < 0 || descriptor < 0 || descriptor >= POSIX_FD_MAX ||
        (access & ~(POSIX_FD_ACCESS_READ | POSIX_FD_ACCESS_WRITE)))
        return -1;
    spin_lock(&posix_fd_lock);
    struct posix_ofd *ofd = descriptor_for_locked((u32)slot, descriptor);
    int result = !ofd ? -1 : ((ofd->access & access) == access ? 0 : -2);
    spin_unlock(&posix_fd_lock);
    return result;
}

int posix_fd_close(struct task *task, int descriptor) {
    int slot = live_task_slot(task);
    if (slot < 0 || descriptor < 0 || descriptor >= POSIX_FD_MAX) return -1;
    spin_lock(&posix_fd_lock);
    if (!descriptor_for_locked((u32)slot, descriptor)) {
        spin_unlock(&posix_fd_lock);
        return -1;
    }
    struct kernel_object *file = detach_descriptor_locked((u32)slot,
                                                           (u32)descriptor);
    spin_unlock(&posix_fd_lock);
    if (file) object_release(file);
    return 0;
}

int posix_fd_dup(struct task *task, int descriptor) {
    int slot = live_task_slot(task);
    if (slot < 0 || descriptor < 0 || descriptor >= POSIX_FD_MAX) return -1;
    spin_lock(&posix_fd_lock);
    struct posix_ofd *ofd = descriptor_for_locked((u32)slot, descriptor);
    u32 replacement = POSIX_FD_MAX;
    for (u32 index = 0; index < POSIX_FD_MAX; index++)
        if (!tables[slot][index].ofd) {
            replacement = index;
            break;
        }
    if (!ofd || replacement == POSIX_FD_MAX || ofd_retain_locked(ofd)) {
        spin_unlock(&posix_fd_lock);
        return -1;
    }
    tables[slot][replacement].ofd = ofd;
    tables[slot][replacement].flags = 0;
    spin_unlock(&posix_fd_lock);
    return (int)replacement;
}

int posix_fd_dup2(struct task *task, int descriptor, int replacement) {
    int slot = live_task_slot(task);
    if (slot < 0 || descriptor < 0 || replacement < 0 ||
        descriptor >= POSIX_FD_MAX || replacement >= POSIX_FD_MAX)
        return -1;
    spin_lock(&posix_fd_lock);
    struct posix_ofd *ofd = descriptor_for_locked((u32)slot, descriptor);
    if (!ofd) {
        spin_unlock(&posix_fd_lock);
        return -1;
    }
    if (descriptor == replacement) {
        spin_unlock(&posix_fd_lock);
        return replacement;
    }
    if (ofd_retain_locked(ofd)) {
        spin_unlock(&posix_fd_lock);
        return -1;
    }
    struct kernel_object *file = 0;
    if (tables[slot][replacement].ofd)
        file = detach_descriptor_locked((u32)slot, (u32)replacement);
    tables[slot][replacement].ofd = ofd;
    tables[slot][replacement].flags = 0;
    spin_unlock(&posix_fd_lock);
    if (file) object_release(file);
    return replacement;
}

int posix_fd_get_cloexec(struct task *task, int descriptor, u32 *enabled) {
    int slot = live_task_slot(task);
    if (slot < 0 || descriptor < 0 || descriptor >= POSIX_FD_MAX || !enabled)
        return -1;
    spin_lock(&posix_fd_lock);
    if (!descriptor_for_locked((u32)slot, descriptor)) {
        spin_unlock(&posix_fd_lock);
        return -1;
    }
    *enabled = tables[slot][descriptor].flags & POSIX_FD_CLOEXEC ? 1 : 0;
    spin_unlock(&posix_fd_lock);
    return 0;
}

int posix_fd_set_cloexec(struct task *task, int descriptor, u32 enabled) {
    int slot = live_task_slot(task);
    if (slot < 0 || descriptor < 0 || descriptor >= POSIX_FD_MAX || enabled > 1)
        return -1;
    spin_lock(&posix_fd_lock);
    if (!descriptor_for_locked((u32)slot, descriptor)) {
        spin_unlock(&posix_fd_lock);
        return -1;
    }
    if (enabled) tables[slot][descriptor].flags |= POSIX_FD_CLOEXEC;
    else tables[slot][descriptor].flags &= ~POSIX_FD_CLOEXEC;
    spin_unlock(&posix_fd_lock);
    return 0;
}

int posix_fd_read(struct task *task, int descriptor, void *buffer,
                  u32 length, u32 *transferred) {
    if (!buffer || !transferred) return -1;
    struct posix_ofd *ofd = retain_descriptor(task, descriptor,
                                               POSIX_FD_ACCESS_READ);
    if (!ofd) return -1;
    spin_lock(&ofd->lock);
    int result = vfs_read(ofd->file, ofd->offset, buffer, length, transferred);
    if (!result) ofd->offset += *transferred;
    spin_unlock(&ofd->lock);
    release_ofd(ofd);
    return result;
}

int posix_fd_write(struct task *task, int descriptor, const void *buffer,
                   u32 length, u32 *transferred) {
    if (!buffer || !transferred) return -1;
    struct posix_ofd *ofd = retain_descriptor(task, descriptor,
                                               POSIX_FD_ACCESS_WRITE);
    if (!ofd) return -1;
    spin_lock(&ofd->lock);
    int result;
    if (ofd->status & POSIX_FD_APPEND) {
        u32 position = 0;
        result = vfs_append(ofd->file, buffer, length, transferred, &position);
        if (!result) ofd->offset = position;
    } else {
        result = vfs_write(ofd->file, ofd->offset, buffer, length, transferred);
        if (!result) ofd->offset += *transferred;
    }
    spin_unlock(&ofd->lock);
    release_ofd(ofd);
    return result;
}

int posix_fd_pread(struct task *task, int descriptor, i64 offset,
                   void *buffer, u32 length, u32 *transferred) {
    if (!buffer || !transferred) return -1;
    // A read past the end of the file answers zero bytes, so an offset
    // beyond the size bound needs no error of its own.
    if (offset < 0) return POSIX_VFS_EINVAL;
    if (offset > (i64)VFS_FILE_SIZE_MAX) {
        *transferred = 0;
        return 0;
    }
    struct posix_ofd *ofd = retain_descriptor(task, descriptor,
                                               POSIX_FD_ACCESS_READ);
    if (!ofd) return -1;
    spin_lock(&ofd->lock);
    int result = vfs_read(ofd->file, (u32)offset, buffer, length, transferred);
    spin_unlock(&ofd->lock);
    release_ofd(ofd);
    return result;
}

int posix_fd_pwrite(struct task *task, int descriptor, i64 offset,
                    const void *buffer, u32 length, u32 *transferred) {
    if (!buffer || !transferred) return -1;
    if (offset < 0) return POSIX_VFS_EINVAL;
    // POSIX lets O_APPEND and pwrite coexist: the explicit offset wins and
    // the shared cursor never moves.
    if (offset > (i64)VFS_FILE_SIZE_MAX ||
        (u64)offset + length > VFS_FILE_SIZE_MAX)
        return POSIX_VFS_EFBIG;
    struct posix_ofd *ofd = retain_descriptor(task, descriptor,
                                               POSIX_FD_ACCESS_WRITE);
    if (!ofd) return -1;
    spin_lock(&ofd->lock);
    int result = vfs_write(ofd->file, (u32)offset, buffer, length, transferred);
    spin_unlock(&ofd->lock);
    release_ofd(ofd);
    return result;
}

int posix_fd_seek(struct task *task, int descriptor, i64 offset, u32 whence,
                  u32 *position) {
    if (!position || whence > 2) return -1;
    struct posix_ofd *ofd = retain_descriptor(task, descriptor, 0);
    if (!ofd) return -1;
    spin_lock(&ofd->lock);
    u32 base = 0;
    int result = 0;
    if (whence == 1) base = ofd->offset;
    else if (whence == 2) {
        struct vfs_node_info info;
        if (vfs_stat(ofd->file, &info)) result = -1;
        else base = info.size;
    }
    if (!result && (offset < -(i64)base ||
                    offset > (i64)VFS_FILE_SIZE_MAX - base))
        result = -1;
    if (!result) {
        ofd->offset = (u32)((i64)base + offset);
        *position = ofd->offset;
    }
    spin_unlock(&ofd->lock);
    release_ofd(ofd);
    return result;
}

// Pack one getdents record: 8-byte inode, 8-byte next-record offset,
// 4-byte record length, 4-byte POSIX_DT_* type, then the NUL-terminated
// name padded to an 8-byte boundary. Returns the padded record length.
static u32 dirent_pack(u8 *record, u64 inode, u64 next, u32 type,
                       const char *name, u32 name_len) {
    u32 length = 24 + name_len + 1;
    length = (length + 7) & ~7;
    for (u32 index = 0; index < 8; index++)
        record[index] = (u8)(inode >> (index * 8));
    for (u32 index = 0; index < 8; index++)
        record[8 + index] = (u8)(next >> (index * 8));
    record[16] = (u8)length;
    record[17] = (u8)(length >> 8);
    record[18] = (u8)(length >> 16);
    record[19] = (u8)(length >> 24);
    record[20] = (u8)type;
    record[21] = 0;
    record[22] = 0;
    record[23] = 0;
    for (u32 index = 0; index < name_len; index++)
        record[24 + index] = (u8)name[index];
    for (u32 index = 24 + name_len; index < length; index++)
        record[index] = 0;
    return length;
}

int posix_fd_getdents(struct task *task, int descriptor, u8 *buffer,
                      u32 length, u32 *transferred) {
    if (!buffer || !transferred || !length || length > POSIX_IO_MAX)
        return POSIX_VFS_EINVAL;
    struct posix_ofd *ofd = retain_descriptor(task, descriptor,
                                               POSIX_FD_ACCESS_READ);
    if (!ofd) return POSIX_VFS_EBADF;
    spin_lock(&ofd->lock);
    u64 cursor = ofd->offset;
    u64 packed = cursor;
    u32 used = 0;
    int failure = 0;
    // Records pack until the next one would overflow the buffer; a buffer
    // that cannot hold even one record is EINVAL, and the cursor only
    // advances past entries that were actually packed.
    while (!failure && used < length) {
        char name[VFS_NAME_MAX + 1];
        u32 name_len = 0;
        u64 entry_inode = 0;
        u32 type = 0;
        int result = vfs_read_dir(ofd->file, &cursor, name, &name_len,
                                  &entry_inode, &type);
        if (result < 0) {
            failure = POSIX_VFS_EIO;
            break;
        }
        if (result > 0) break;
        u32 record_length = 24 + name_len + 1;
        record_length = (record_length + 7) & ~7;
        if (used + record_length > length) {
            if (!used) failure = POSIX_VFS_EINVAL;
            break;
        }
        dirent_pack(buffer + used, entry_inode, cursor, type, name, name_len);
        used += record_length;
        packed = cursor;
    }
    if (!failure) ofd->offset = (u32)packed;
    spin_unlock(&ofd->lock);
    release_ofd(ofd);
    if (failure) return failure;
    *transferred = used;
    return 0;
}

int posix_fd_ftruncate(struct task *task, int descriptor, i64 length) {
    // POSIX folds a descriptor that is not open for writing into EBADF
    // rather than EACCES, unlike the read and write paths.
    if (length < 0) return POSIX_VFS_EINVAL;
    if (length > (i64)VFS_FILE_SIZE_MAX) return POSIX_VFS_EFBIG;
    struct posix_ofd *ofd = retain_descriptor(task, descriptor,
                                               POSIX_FD_ACCESS_WRITE);
    if (!ofd) return POSIX_VFS_EBADF;
    int result = vfs_truncate(ofd->file, (u32)length);
    release_ofd(ofd);
    return result ? POSIX_VFS_EIO : 0;
}

// fdatasync may skip metadata a later read does not need, but the format
// lands data and metadata in a single commit window, so both calls ride the
// same staging path. The commit flushes the block cache and flips the
// generation, which is durable as far as the emulated disk goes; a real
// device write cache would still need a flush command, and that barrier is
// not built yet.
int posix_fd_fsync(struct task *task, int descriptor) {
    struct posix_ofd *ofd = retain_descriptor(task, descriptor, 0);
    if (!ofd) return -1;
    int result = vfs_fsync(ofd->file);
    release_ofd(ofd);
    return result;
}

int posix_fd_stat(struct task *task, int descriptor, struct vfs_node_info *info) {
    if (!info) return -1;
    struct posix_ofd *ofd = retain_descriptor(task, descriptor, 0);
    if (!ofd) return -1;
    int result = vfs_stat(ofd->file, info);
    release_ofd(ofd);
    return result;
}

int posix_fd_fchmod(struct task *task, int descriptor, u32 mode) {
    if (mode & ~VFS_MODE_MASK) return POSIX_VFS_EINVAL;
    if (!posix_profile_vfs_authorized(task)) return POSIX_VFS_EACCES;
    struct posix_ofd *ofd = retain_descriptor(task, descriptor, 0);
    if (!ofd) return POSIX_VFS_EBADF;
    struct vfs_node_info info;
    int result = vfs_stat(ofd->file, &info) ? POSIX_VFS_EIO : 0;
    if (!result && info.readonly) result = POSIX_VFS_EROFS;
    if (!result && info.uid != task->uid) result = POSIX_VFS_EPERM;
    if (!result && vfs_chmod(ofd->file, mode)) result = POSIX_VFS_EIO;
    release_ofd(ofd);
    return result;
}

int posix_fd_fork(struct task *parent, struct task *child) {
    int parent_slot = live_task_slot(parent);
    int child_slot = live_task_slot(child);
    if (parent_slot < 0 || child_slot < 0 || parent_slot == child_slot)
        return -1;
    spin_lock(&posix_fd_lock);
    for (u32 index = 0; index < POSIX_FD_MAX; index++) {
        if (tables[child_slot][index].ofd) {
            spin_unlock(&posix_fd_lock);
            return -1;
        }
        struct posix_ofd *ofd = tables[parent_slot][index].ofd;
        if (ofd && (!ofd->active || !ofd->references ||
                    ofd->references == 0xFFFFFFFFu)) {
            spin_unlock(&posix_fd_lock);
            return -1;
        }
    }
    for (u32 index = 0; index < POSIX_FD_MAX; index++) {
        struct posix_ofd *ofd = tables[parent_slot][index].ofd;
        if (!ofd) continue;
        ofd->references++;
        tables[child_slot][index].ofd = ofd;
        tables[child_slot][index].flags = tables[parent_slot][index].flags;
    }
    spin_unlock(&posix_fd_lock);
    return 0;
}

static void close_descriptors(struct task *task, int cloexec_only) {
    int slot = task_slot(task);
    if (slot < 0) return;
    struct kernel_object *files[POSIX_FD_MAX];
    u32 count = 0;
    spin_lock(&posix_fd_lock);
    for (u32 index = 0; index < POSIX_FD_MAX; index++) {
        if (!tables[slot][index].ofd ||
            (cloexec_only && !(tables[slot][index].flags & POSIX_FD_CLOEXEC)))
            continue;
        struct kernel_object *file = detach_descriptor_locked((u32)slot, index);
        if (file) files[count++] = file;
    }
    spin_unlock(&posix_fd_lock);
    release_files(files, count);
}

void posix_fd_close_cloexec(struct task *task) {
    close_descriptors(task, 1);
}

void posix_fd_close_all(struct task *task) {
    close_descriptors(task, 0);
}

u32 posix_fd_revoke_object(struct kernel_object *object) {
    if (!object) return 0;
    struct kernel_object *files[POSIX_FD_MAX];
    u32 count = 0;
    u32 revoked = 0;
    spin_lock(&posix_fd_lock);
    for (u32 task = 0; task < MAX_TASKS; task++) {
        for (u32 descriptor = 0; descriptor < POSIX_FD_MAX; descriptor++) {
            struct posix_ofd *ofd = tables[task][descriptor].ofd;
            if (!ofd || ofd->file != object) continue;
            struct kernel_object *file = detach_descriptor_locked(task,
                                                                   descriptor);
            if (file) files[count++] = file;
            revoked++;
        }
    }
    spin_unlock(&posix_fd_lock);
    release_files(files, count);
    return revoked;
}

u32 posix_fd_active_count(void) {
    u32 count = 0;
    spin_lock(&posix_fd_lock);
    for (u32 task = 0; task < MAX_TASKS; task++)
        for (u32 descriptor = 0; descriptor < POSIX_FD_MAX; descriptor++)
            if (tables[task][descriptor].ofd) count++;
    spin_unlock(&posix_fd_lock);
    return count;
}

u32 posix_ofd_active_count(void) {
    u32 count = 0;
    spin_lock(&posix_fd_lock);
    for (u32 index = 0; index < POSIX_OFD_MAX; index++)
        if (ofds[index].active) count++;
    spin_unlock(&posix_fd_lock);
    return count;
}
