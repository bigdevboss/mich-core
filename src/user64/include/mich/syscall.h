#ifndef MICH64_USER_SYSCALL_H
#define MICH64_USER_SYSCALL_H

#define MICH_SYS_WRITE 1
#define MICH_SYS_MEMFREE 11
#define MICH_SYS_FORK 12
#define MICH_SYS_EXEC 13
#define MICH_SYS_EXIT 14
#define MICH_SYS_WAIT 15
#define MICH_SYS_GETPID 16
#define MICH_SYS_YIELD 23
#define MICH_SYS_SPAWN 32
#define MICH_SYS_POSIX_OPEN 186
#define MICH_SYS_POSIX_CLOSE 187
#define MICH_SYS_POSIX_READ 188
#define MICH_SYS_POSIX_WRITE 189
#define MICH_SYS_POSIX_LSEEK 190
#define MICH_SYS_POSIX_DUP 191
#define MICH_SYS_POSIX_DUP2 192
#define MICH_SYS_POSIX_FCNTL 193
#define MICH_SYS_POSIX_STAT 194
#define MICH_SYS_POSIX_FSTAT 195
#define MICH_SYS_POSIX_MKDIR 196
#define MICH_SYS_POSIX_RMDIR 197
#define MICH_SYS_POSIX_UNLINK 198
#define MICH_SYS_POSIX_CHDIR 199
#define MICH_SYS_POSIX_GETCWD 200
#define MICH_SYS_POSIX_TRUNCATE 201
#define MICH_SYS_POSIX_BRK 208

long mich_syscall0(unsigned long number);
long mich_syscall1(unsigned long number, unsigned long arg0);
long mich_syscall2(unsigned long number, unsigned long arg0,
                   unsigned long arg1);
long mich_syscall3(unsigned long number, unsigned long arg0,
                   unsigned long arg1, unsigned long arg2);
int mich_write(const char *text);
int mich_memfree(void);
void mich_exit(int code) __attribute__((noreturn));
int mich_wait(int pid);
int mich_getpid(void);
int mich_kill(int pid);
int mich_yield(void);
int mich_fork(void);
int mich_exec(const char *path, unsigned long argument);
int mich_spawn(unsigned long argument);

#endif
