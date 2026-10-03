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
/* 210 through 216 belong to the driver-domain socket stream calls, so the
   first free number for the POSIX chain is 217. */
#define POSIX_SYSCALL_GETDENTS 217u
#define POSIX_SYSCALL_CHMOD 218u
#define POSIX_SYSCALL_FCHMOD 219u
#define POSIX_SYSCALL_CHOWN 220u
#define POSIX_SYSCALL_UMASK 221u
#define POSIX_SYSCALL_LINK 222u
#define POSIX_SYSCALL_RENAME 223u
#define POSIX_SYSCALL_SYMLINK 224u
#define POSIX_SYSCALL_READLINK 225u
#define POSIX_SYSCALL_LSTAT 226u
#define POSIX_SYSCALL_FTRUNCATE 227u
#define POSIX_SYSCALL_PREAD 228u
#define POSIX_SYSCALL_PWRITE 229u
#define POSIX_SYSCALL_FSYNC 230u
#define POSIX_SYSCALL_FDATASYNC 231u

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

// getdents packs variable-length records into the data buffer: an 8-byte
// inode, an 8-byte offset for the next record, a 4-byte record length, a
// 4-byte POSIX_DT_* type, then the NUL-terminated name padded to 8 bytes.
#define POSIX_DT_REG 1u
#define POSIX_DT_DIR 2u
#define POSIX_DT_LNK 3u

struct posix_getdents_request {
    i32 descriptor;
    u32 length;
    u32 transferred;
    u8 data[POSIX_IO_MAX];
};

struct posix_chmod_request {
    u32 mode;
    char path[VFS_PATH_MAX];
};

struct posix_fchmod_request {
    i32 descriptor;
    u32 mode;
    u32 reserved;
};

// chown carries uid and gid as signed values where -1 leaves the field
// unchanged, the POSIX convention.
struct posix_chown_request {
    i32 uid;
    i32 gid;
    char path[VFS_PATH_MAX];
};

struct posix_umask_request {
    u32 mask;
    u32 reserved;
};

struct posix_link_request {
    char old_path[VFS_PATH_MAX];
    char new_path[VFS_PATH_MAX];
};

struct posix_rename_request {
    char old_path[VFS_PATH_MAX];
    char new_path[VFS_PATH_MAX];
};

struct posix_symlink_request {
    char target[VFS_PATH_MAX];
    char path[VFS_PATH_MAX];
};

// readlink fills the data buffer and the length; the copy back to the
// caller carries both, the way getdents does.
struct posix_readlink_request {
    char path[VFS_PATH_MAX];
    u32 length;
    char data[VFS_PATH_MAX];
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
    u32 st_uid;
    u32 st_gid;
    u64 st_atime;
    u64 st_mtime;
    u64 st_ctime;
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

// ftruncate takes a signed length because POSIX makes a negative one
// EINVAL; the path-based truncate above keeps its unsigned ABI.
struct posix_ftruncate_request {
    i32 descriptor;
    u32 reserved;
    i64 length;
};

// pread and pwrite address the file at an explicit offset and leave the
// descriptor position alone, so concurrent operations never interleave
// through a shared cursor.
struct posix_pio_request {
    i32 descriptor;
    u32 reserved;
    i64 offset;
    u32 length;
    u8 data[POSIX_IO_MAX];
};

#endif
