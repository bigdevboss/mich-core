#ifndef POSIX_ABI_H
#define POSIX_ABI_H

#include "types.h"
#include "vfs.h"

#define POSIX_SYSCALL_OPEN 186u
#define POSIX_SYSCALL_CLOSE 187u
#define POSIX_SYSCALL_READ 188u
#define POSIX_SYSCALL_WRITE 189u
#define POSIX_SYSCALL_LSEEK 190u
#define POSIX_SYSCALL_DUP 191u
#define POSIX_SYSCALL_DUP2 192u
#define POSIX_SYSCALL_FCNTL 193u
#define POSIX_SYSCALL_STAT 194u
#define POSIX_SYSCALL_FSTAT 195u
#define POSIX_SYSCALL_MKDIR 196u
#define POSIX_SYSCALL_RMDIR 197u
#define POSIX_SYSCALL_UNLINK 198u
#define POSIX_SYSCALL_CHDIR 199u
#define POSIX_SYSCALL_GETCWD 200u
#define POSIX_SYSCALL_TRUNCATE 201u
#define POSIX_SYSCALL_FORK 202u
#define POSIX_SYSCALL_EXECVE 203u
#define POSIX_SYSCALL_EXIT 204u
#define POSIX_SYSCALL_WAITPID 205u
#define POSIX_SYSCALL_GETPID 206u
#define POSIX_SYSCALL_GETPPID 207u
#define POSIX_SYSCALL_BRK 208u
#define POSIX_SYSCALL_GETRANDOM 209u

#define POSIX_IO_MAX 512u
#define POSIX_SEEK_SET 0u
#define POSIX_SEEK_CUR 1u
#define POSIX_SEEK_END 2u
#define POSIX_FCNTL_GETFD 1u
#define POSIX_FCNTL_SETFD 2u
#define POSIX_FD_CLOEXEC_VALUE 1u
#define POSIX_WAIT_NOHANG 1u

struct posix_open_request {
    u32 flags;
    u32 mode;
    char path[VFS_PATH_MAX];
};

struct posix_fd_request {
    i32 descriptor;
    u32 reserved;
};

struct posix_io_request {
    i32 descriptor;
    u32 length;
    u32 transferred;
    u8 data[POSIX_IO_MAX];
};

struct posix_seek_request {
    i32 descriptor;
    u32 whence;
    i64 offset;
    u32 position;
    u32 reserved;
};

struct posix_dup2_request {
    i32 descriptor;
    i32 replacement;
};

struct posix_fcntl_request {
    i32 descriptor;
    u32 command;
    u32 argument;
    u32 reserved;
};

struct posix_stat_record {
    u32 st_mode;
    u32 st_size;
    u32 st_nlink;
    u32 st_reserved;
};

struct posix_stat_path_request {
    char path[VFS_PATH_MAX];
    struct posix_stat_record stat;
};

struct posix_fstat_request {
    i32 descriptor;
    u32 reserved;
    struct posix_stat_record stat;
};

struct posix_mode_path_request {
    u32 mode;
    char path[VFS_PATH_MAX];
};

struct posix_path_request {
    char path[VFS_PATH_MAX];
};

struct posix_getcwd_request {
    u32 capacity;
    u32 length;
    char path[VFS_PATH_MAX];
};

struct posix_truncate_request {
    u32 size;
    char path[VFS_PATH_MAX];
};

#endif
