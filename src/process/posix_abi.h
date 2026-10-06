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
#define POSIX_SYSCALL_SOCKET 246u
#define POSIX_SYSCALL_BIND 247u
#define POSIX_SYSCALL_LISTEN 248u
#define POSIX_SYSCALL_ACCEPT 249u
#define POSIX_SYSCALL_CONNECT 250u
#define POSIX_SYSCALL_SEND 251u
#define POSIX_SYSCALL_RECV 252u
#define POSIX_SYSCALL_SENDTO 253u
#define POSIX_SYSCALL_RECVFROM 254u
#define POSIX_SYSCALL_SENDMSG 255u
#define POSIX_SYSCALL_RECVMSG 256u
#define POSIX_SYSCALL_SHUTDOWN 257u
#define POSIX_SYSCALL_GETSOCKOPT 258u
#define POSIX_SYSCALL_SETSOCKOPT 259u
#define POSIX_SYSCALL_GETSOCKNAME 260u
#define POSIX_SYSCALL_GETPEERNAME 261u
#define POSIX_SYSCALL_POLL 262u

// The bounded signal set the profile carries. The numbers are the POSIX
// ones; everything outside this list is rejected as EINVAL rather than
// mapped onto a near relative. The set now carries the job-control five
// (CONT, STOP, TSTP, TTIN, TTOU); realtime signals stay out, since
// nothing here queues more than one instance of a signal.
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
#define POSIX_SIG_CONT 18u
#define POSIX_SIG_STOP 19u
#define POSIX_SIG_TSTP 20u
#define POSIX_SIG_TTIN 21u
#define POSIX_SIG_TTOU 22u
// The table is indexed by signal number, so the bound is the highest
// number the set reaches and not the count of members: everything above
// the last one is EINVAL at the dispatcher.
#define POSIX_SIG_COUNT 22u

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
// A stopped or continued child is reported only when the caller asked for
// it, which is what these two options spell.
#define POSIX_WAIT_UNTRACED 2u
#define POSIX_WAIT_CONTINUED 4u
// The status word the profile packs into the caller's int: a low byte of
// zero means an exit (code shifted up, what the profile always did), a
// nonzero low byte below 0x7F means a death carrying the signal there, and
// the two reports that are not a death take the low byte whole so no exit
// code can ever be mistaken for one: a stop reads 0x7F with the stopping
// signal below it, a continue reads all ones. This is the Linux shape,
// which is what a caller carrying Linux habits already tests for.
#define POSIX_WAIT_STATUS_STOPPED 0x7Fu
#define POSIX_WAIT_STATUS_CONTINUED 0xFFFFu
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

// The socket address the profile answers to: AF_INET only, the same 16
// byte layout the userkit headers carry, with the port and address in
// network byte order the way POSIX defines them.
#define POSIX_AF_INET 2u
#define POSIX_SOCK_STREAM 1u
#define POSIX_SOCK_DGRAM 2u
#define POSIX_SOCKADDR_IN_SIZE 16u

struct posix_sockaddr_in {
    u16 family;
    u16 port;
    u32 address;
    u8 zero[8];
};

struct posix_socket_request {
    u32 domain;
    u32 type;
    u32 protocol;
    u32 reserved;
};

// bind and connect carry one address in; the name calls carry one out and
// report the filled length through the same word.
struct posix_socket_address_request {
    i32 descriptor;
    u32 reserved;
    struct posix_sockaddr_in address;
    u32 length;
};

struct posix_socket_listen_request {
    i32 descriptor;
    u32 backlog;
    u32 reserved;
};

struct posix_socket_accept_request {
    i32 descriptor;
    u32 reserved;
};

// The data calls stage their payload inline the way the io pair does, so
// one datagram or one stream chunk rides in a single copied request. A
// stream buffer larger than POSIX_IO_MAX arrives as several calls; a
// datagram larger than the staging buffer is EMSGSIZE rather than a cut.
struct posix_socket_io_request {
    i32 descriptor;                    // offset 0
    u32 flags;                         // offset 4
    struct posix_sockaddr_in address;  // offset 8
    u32 address_length;                // offset 24
    u32 length;                        // offset 28
    u32 transferred;                   // offset 32
    u32 reserved;                      // offset 36
    u8 data[POSIX_IO_MAX];             // offset 40
};

#define POSIX_SOCKET_IO_TRANSFERRED_OFFSET 32u
#define POSIX_SOCKET_IO_DATA_OFFSET 40u

// The message calls gather and scatter through caller vectors the kernel
// chases per segment; control data has no v0 meaning and must be empty.
#define POSIX_MSG_IOV_MAX 4u

struct posix_iovec {
    uptr_t base;
    u32 length;
    u32 reserved;
};

struct posix_msghdr {
    i32 descriptor;
    u32 flags_in;
    struct posix_sockaddr_in address;
    u32 address_length;
    u32 iov_count;
    struct posix_iovec iov[POSIX_MSG_IOV_MAX];
    u32 control_length;
    u32 flags;
    u64 reserved;
};

struct posix_sockopt_request {
    i32 descriptor;
    u32 level;
    u32 name;
    u32 length;
    u32 reserved;
    u8 value[32];
};

struct posix_socket_shutdown_request {
    i32 descriptor;
    u32 how;
    u32 reserved;
};

// poll event bits, the POSIX values. The caller asks with the low two and
// the kernel answers with those it found plus the error bits, which are
// always reported: a caller that asked only for readability still learns
// the write end died.
#define POSIX_POLLIN 0x001u
#define POSIX_POLLOUT 0x004u
#define POSIX_POLLERR 0x008u
#define POSIX_POLLHUP 0x010u
#define POSIX_POLLNVAL 0x020u

// The bits a caller may ask for; the error bits are answers only and a
// request carrying one is a bug on the caller's side, not a silent no-op.
#define POSIX_POLL_REQUEST_MASK (POSIX_POLLIN | POSIX_POLLOUT)

struct posix_poll_fd {
    i32 descriptor;
    u16 events;
    u16 revents;
};

// The descriptor array rides inline in the copied request; a poll list
// longer than the descriptor table could only repeat it, and the timeout
// arrives as the second syscall argument so -1 means wait forever.
#define POSIX_POLL_FD_MAX 16u

struct posix_poll_request {
    u32 count;
    u32 reserved;
    struct posix_poll_fd fds[POSIX_POLL_FD_MAX];
};

// ioctl carries the tty requests. The numbers are the Linux ones so a
// ported program passes them unchanged; the kernel answers ENOTTY for a
// request that does not belong to the descriptor it was aimed at.
#define POSIX_SYSCALL_IOCTL 263u
#define POSIX_SYSCALL_GETPGRP 264u
// arg0 is the pid (0 means the caller), arg1 the group (0 means the pid).
#define POSIX_SYSCALL_SETPGID 265u
#define POSIX_SYSCALL_TCGETPGRP 266u
#define POSIX_SYSCALL_TCSETPGRP 267u

#define POSIX_TCGETS 0x5401u
#define POSIX_TCSETS 0x5402u
#define POSIX_TIOCGPGRP 0x540Fu
#define POSIX_TIOCSPGRP 0x5410u
#define POSIX_TIOCGWINSZ 0x5413u
#define POSIX_TIOCSWINSZ 0x5414u

// The termios layout the profile carries: the four flag words, the line
// discipline selector, and the control characters. The array keeps the
// Linux indices so the classic VINTR/VERASE spellings line up; the slots
// the line discipline does not use stay as the caller left them.
#define POSIX_NCCS 19u
#define POSIX_VINTR 0u
#define POSIX_VQUIT 1u
#define POSIX_VERASE 2u
#define POSIX_VKILL 3u
#define POSIX_VEOF 4u
#define POSIX_VSUSP 10u

struct posix_termios {
    u32 iflag;
    u32 oflag;
    u32 cflag;
    u32 lflag;
    u8 line;
    u8 cc[POSIX_NCCS];
};

// Input flags: CR to NL is the one every interactive program relies on.
#define POSIX_ICRNL 0x100u
// Output flags: NL to CR-NL keeps a raw line feed from staircasing a
// terminal dialed in over the serial port.
#define POSIX_OPOST 0x1u
#define POSIX_ONLCR 0x4u
// Local flags: the three the line discipline implements. Everything else
// a caller sets is refused rather than half-honored.
#define POSIX_ISIG 0x1u
#define POSIX_ICANON 0x2u
#define POSIX_ECHO 0x8u

struct posix_winsize {
    u16 rows;
    u16 cols;
    u16 xpixel;
    u16 ypixel;
};

#endif
