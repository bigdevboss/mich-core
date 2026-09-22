#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <mich/syscall.h>
#include <posix_abi.h>

int errno;

static int copy_path(char destination[VFS_PATH_MAX], const char *source) {
    if (!source) return -EINVAL;
    for (u32 index = 0; index < VFS_PATH_MAX; index++) {
        destination[index] = source[index];
        if (!source[index]) return 0;
    }
    return -ENAMETOOLONG;
}

static long request_call(u32 number, void *request) {
    return mich_syscall1(number, (unsigned long)request);
}

static int result_int(long result) {
    if (result >= 0) return (int)result;
    errno = (int)-result;
    return -1;
}

int open(const char *path, int flags, ...) {
    struct posix_open_request request;
    for (u32 index = 0; index < sizeof(request); index++) ((u8 *)&request)[index] = 0;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    request.flags = (u32)flags;
    if (flags & O_CREAT) {
        __builtin_va_list arguments;
        __builtin_va_start(arguments, flags);
        request.mode = __builtin_va_arg(arguments, unsigned int);
        __builtin_va_end(arguments);
    }
    return result_int(request_call(POSIX_SYSCALL_OPEN, &request));
}

int close(int fd) {
    struct posix_fd_request request = { fd, 0 };
    return result_int(request_call(POSIX_SYSCALL_CLOSE, &request));
}

static ssize_t io_call(u32 number, int fd, void *buffer, size_t length) {
    if (!buffer && length) {
        errno = EINVAL;
        return -1;
    }
    size_t done = 0;
    while (done < length) {
        u32 chunk = length - done > POSIX_IO_MAX ? POSIX_IO_MAX :
            (u32)(length - done);
        struct posix_io_request request;
        request.descriptor = fd;
        request.length = chunk;
        request.transferred = 0;
        if (number == POSIX_SYSCALL_WRITE)
            for (u32 index = 0; index < chunk; index++)
                request.data[index] = ((const u8 *)buffer)[done + index];
        long result = request_call(number, &request);
        if (result < 0) {
            if (done) return (ssize_t)done;
            errno = (int)-result;
            return -1;
        }
        if (number == POSIX_SYSCALL_READ)
            for (u32 index = 0; index < (u32)result; index++)
                ((u8 *)buffer)[done + index] = request.data[index];
        done += (u32)result;
        if ((u32)result < chunk) break;
    }
    return (ssize_t)done;
}

ssize_t read(int fd, void *buffer, size_t length) {
    return io_call(POSIX_SYSCALL_READ, fd, buffer, length);
}

ssize_t write(int fd, const void *buffer, size_t length) {
    return io_call(POSIX_SYSCALL_WRITE, fd, (void *)buffer, length);
}

off_t lseek(int fd, off_t offset, int whence) {
    struct posix_seek_request request;
    request.descriptor = fd;
    request.whence = (u32)whence;
    request.offset = offset;
    request.position = 0;
    request.reserved = 0;
    long result = request_call(POSIX_SYSCALL_LSEEK, &request);
    if (result < 0) {
        errno = (int)-result;
        return (off_t)-1;
    }
    return (off_t)result;
}

int dup(int fd) {
    struct posix_fd_request request = { fd, 0 };
    return result_int(request_call(POSIX_SYSCALL_DUP, &request));
}

int dup2(int fd, int replacement) {
    struct posix_dup2_request request = { fd, replacement };
    return result_int(request_call(POSIX_SYSCALL_DUP2, &request));
}

int fcntl(int fd, int command, ...) {
    struct posix_fcntl_request request;
    request.descriptor = fd;
    request.command = (u32)command;
    request.argument = 0;
    request.reserved = 0;
    if (command == F_SETFD) {
        __builtin_va_list arguments;
        __builtin_va_start(arguments, command);
        request.argument = __builtin_va_arg(arguments, unsigned int);
        __builtin_va_end(arguments);
    }
    return result_int(request_call(POSIX_SYSCALL_FCNTL, &request));
}

static void copy_stat(struct stat *destination,
                      const struct posix_stat_record *source) {
    destination->st_mode = source->st_mode;
    destination->st_size = source->st_size;
    destination->st_nlink = source->st_nlink;
    destination->st_reserved = source->st_reserved;
}

int stat(const char *path, struct stat *buffer) {
    if (!buffer) {
        errno = EINVAL;
        return -1;
    }
    struct posix_stat_path_request request;
    for (u32 index = 0; index < sizeof(request); index++) ((u8 *)&request)[index] = 0;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    long result = request_call(POSIX_SYSCALL_STAT, &request);
    if (result < 0) return result_int(result);
    copy_stat(buffer, &request.stat);
    return 0;
}

int fstat(int fd, struct stat *buffer) {
    if (!buffer) {
        errno = EINVAL;
        return -1;
    }
    struct posix_fstat_request request;
    request.descriptor = fd;
    request.reserved = 0;
    request.stat.st_mode = 0;
    request.stat.st_size = 0;
    request.stat.st_nlink = 0;
    request.stat.st_reserved = 0;
    long result = request_call(POSIX_SYSCALL_FSTAT, &request);
    if (result < 0) return result_int(result);
    copy_stat(buffer, &request.stat);
    return 0;
}

int mkdir(const char *path, mode_t mode) {
    struct posix_mode_path_request request;
    for (u32 index = 0; index < sizeof(request); index++) ((u8 *)&request)[index] = 0;
    request.mode = mode;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    return result_int(request_call(POSIX_SYSCALL_MKDIR, &request));
}

static int path_call(u32 number, const char *path) {
    struct posix_path_request request;
    for (u32 index = 0; index < sizeof(request); index++) request.path[index] = 0;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    return result_int(request_call(number, &request));
}

int rmdir(const char *path) {
    return path_call(POSIX_SYSCALL_RMDIR, path);
}

int unlink(const char *path) {
    return path_call(POSIX_SYSCALL_UNLINK, path);
}

int chdir(const char *path) {
    return path_call(POSIX_SYSCALL_CHDIR, path);
}

char *getcwd(char *buffer, size_t size) {
    if (!buffer || !size) {
        errno = EINVAL;
        return 0;
    }
    struct posix_getcwd_request request;
    for (u32 index = 0; index < sizeof(request); index++) ((u8 *)&request)[index] = 0;
    request.capacity = size > VFS_PATH_MAX ? VFS_PATH_MAX : (u32)size;
    long result = request_call(POSIX_SYSCALL_GETCWD, &request);
    if (result < 0) {
        errno = (int)-result;
        return 0;
    }
    for (u32 index = 0; index <= request.length; index++) buffer[index] = request.path[index];
    return buffer;
}

ssize_t getrandom(void *buffer, size_t length, unsigned int flags) {
    long result = mich_syscall3(POSIX_SYSCALL_GETRANDOM,
                                (unsigned long)buffer, (unsigned long)length,
                                (unsigned long)flags);
    if (result >= 0) return (ssize_t)result;
    errno = (int)-result;
    return -1;
}

int truncate(const char *path, off_t size) {
    if (size < 0 || (u64)size > VFS_FILE_SIZE_MAX) {
        errno = size < 0 ? EINVAL : EFBIG;
        return -1;
    }
    struct posix_truncate_request request;
    for (u32 index = 0; index < sizeof(request); index++) ((u8 *)&request)[index] = 0;
    request.size = (u32)size;
    int copied = copy_path(request.path, path);
    if (copied) return result_int(copied);
    return result_int(request_call(POSIX_SYSCALL_TRUNCATE, &request));
}
