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
// 210 through 216 belong to the driver-domain socket stream calls, so the
// first free number for the POSIX chain is 217.
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
#define POSIX_SYSCALL_ACCESS 232u
#define POSIX_SYSCALL_UTIMENSAT 233u
#define POSIX_SYSCALL_FUTIMENS 234u
#define POSIX_SYSCALL_CLOCK_GETTIME 235u
#define POSIX_SYSCALL_CLOCK_GETRES 236u
#define POSIX_SYSCALL_NANOSLEEP 237u
#define POSIX_SYSCALL_KILL 238u
#define POSIX_SYSCALL_SIGACTION 239u
#define POSIX_SYSCALL_SIGPROCMASK 240u
#define POSIX_SYSCALL_SIGRETURN 241u
#define POSIX_SYSCALL_SIGPENDING 242u
#define POSIX_SYSCALL_PLEDGE 243u
#define POSIX_SYSCALL_UNVEIL 244u
#define POSIX_SYSCALL_PIPE 245u

// The bounded signal set the profile carries. The numbers are the POSIX
// ones; everything outside this list is rejected as EINVAL rather than
// mapped onto a near relative. Job control (SIGSTOP, SIGCONT) and realtime
// signals stay out: there is no process group or queued signal semantics
// to hang them on.
#define POSIX_SIG_HUP 1u
#define POSIX_SIG_INT 2u
#define POSIX_SIG_QUIT 3u
#define POSIX_SIG_ILL 4u
#define POSIX_SIG_ABRT 6u
#define POSIX_SIG_FPE 8u
#define POSIX_SIG_KILL 9u
#define POSIX_SIG_USR1 10u
#define POSIX_SIG_SEGV 11u
#define POSIX_SIG_USR2 12u
#define POSIX_SIG_PIPE 13u
#define POSIX_SIG_ALRM 14u
#define POSIX_SIG_TERM 15u
#define POSIX_SIG_CHLD 17u
#define POSIX_SIG_COUNT 32u

// Dispositions: the default action, explicit ignore, or a handler address.
// Anything above one is a user instruction pointer.
#define POSIX_SIG_DFL ((uptr_t)0)
#define POSIX_SIG_IGN ((uptr_t)1)

// The sigaction flags the kernel reads. SA_APPLY marks a real disposition
// change: without it the call only reads the current one back, which is
// how a null act pointer queries. The restorer ride is mandatory because
// the kernel has no sigreturn trampoline of its own.
#define POSIX_SA_APPLY 0x00000001u
#define POSIX_SA_RESTORER 0x04000000u

// sigprocmask operations, with the Linux values applications expect.
#define POSIX_SIG_BLOCK 0u
#define POSIX_SIG_UNBLOCK 1u
#define POSIX_SIG_SETMASK 2u

// sigaction copies the new disposition in and the previous one out, so one
// request record serves both pointers of the libc wrapper.
struct posix_sigaction_request {
    i32 signo;
    u32 flags;
    uptr_t handler;
    uptr_t restorer;
    u64 mask;
    uptr_t previous_handler;
    uptr_t previous_restorer;
    u64 previous_mask;
    u32 previous_flags;
    u32 reserved;
};

// sigprocmask reads the requested set and answers with the previous one.
struct posix_sigprocmask_request {
    u32 how;
    u32 reserved;
    u64 mask;
    u64 previous;
};

// sigpending answers with the pending signals the caller has blocked.
struct posix_sigpending_request {
    u64 pending;
};

// The frame a delivery pushes onto the user stack. It carries the whole
// interrupted register set because a tick can land between any two user
// instructions, not only at a syscall boundary where the architecture
// already burns rcx and r11. The restorer reads it back through sigreturn.
struct posix_sigframe {
    uptr_t restorer;
    u64 signo;
    u64 saved_mask;
    u64 rax;
    u64 rcx;
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 rip;
    u64 rsp;
    u64 rflags;
};

// sigreturn cannot hand its restored frame back through the plain sysret
// path: sysret forces rcx to the return rip and r11 to the flags, which
// would clobber two registers the interrupted code still owned. The
// dispatcher returns this sentinel instead and the entry stub takes an iret
// path that reloads rcx, r11, and rax from the per-CPU save slot. Real
// syscall results are small non-negative numbers or negative errno, so a
// 64-bit sentinel cannot collide.
#define POSIX_SIGRETURN_SENTINEL 0x5349475245544952ull

#define POSIX_IO_MAX 512u
#define POSIX_SEEK_SET 0u
#define POSIX_SEEK_CUR 1u
#define POSIX_SEEK_END 2u
#define POSIX_FCNTL_GETFD 1u
#define POSIX_FCNTL_SETFD 2u
#define POSIX_FD_CLOEXEC_VALUE 1u
#define POSIX_WAIT_NOHANG 1u
// The two reserved tv_nsec spellings: stamp the current time or leave the
// field alone. tv_sec is ignored whenever a value carries either one.
#define POSIX_UTIME_NOW 0x3FFFFFFEu
#define POSIX_UTIME_OMIT 0x3FFFFFFFu

struct posix_open_request {
    u32 flags;
    u32 mode;
    char path[VFS_PATH_MAX];
};

// Pledge promise strings: the canon-9 vocabulary with separators stays
// far below this bound; a longer promise list answers EINVAL because the
// trailing terminator never lands inside the buffer.
#define POSIX_PLEDGE_PROMISE_MAX 96u

// reserved doubles as a presence map: an absent promise string means
// "leave that half unchanged" while an empty one is a real empty set, and
// only the flags can tell them apart over a copied buffer.
#define POSIX_PLEDGE_HAS_PROMISES 1u
#define POSIX_PLEDGE_HAS_EXEC_PROMISES 2u
#define POSIX_UNVEIL_LOCK 1u

struct posix_pledge_request {
    char promises[POSIX_PLEDGE_PROMISE_MAX];
    char execpromises[POSIX_PLEDGE_PROMISE_MAX];
    u32 flags;
};

struct posix_unveil_request {
    char path[VFS_PATH_MAX];
    char permissions[8];
    u32 flags;
};

// The kernel answers the two descriptors in place: zero on success with
// descriptors[0] the read end and descriptors[1] the write end.
struct posix_pipe_request {
    i32 descriptors[2];
    u32 reserved;
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

// The mode carries the POSIX R_OK, W_OK, X_OK set with F_OK as a bare
// existence probe; the constants live with the resolver in posix_vfs.h.
struct posix_access_request {
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
    u32 st_atime_nsec;
    u32 st_mtime_nsec;
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

struct posix_utimensat_request {
    i64 atime_sec;
    i64 atime_nsec;
    i64 mtime_sec;
    i64 mtime_nsec;
    char path[VFS_PATH_MAX];
};

struct posix_futimens_request {
    i32 descriptor;
    u32 reserved;
    i64 atime_sec;
    i64 atime_nsec;
    i64 mtime_sec;
    i64 mtime_nsec;
};

// clock_gettime and clock_getres answer with seconds plus nanoseconds for
// the clock the request names. The kernel answer moves in timer ticks, so
// getres reports one tick rather than pretending at finer granularity.
struct posix_clock_request {
    u32 clock;
    u32 reserved;
    i64 sec;
    i64 nsec;
};

// The interval is copied in, the remaining interval copied out, which keeps
// one user pointer pair out of the ABI. Nothing reports a remainder yet:
// the sleep has no interrupt source until signals arrive.
struct posix_nanosleep_request {
    i64 sec;
    i64 nsec;
    i64 remaining_sec;
    i64 remaining_nsec;
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
