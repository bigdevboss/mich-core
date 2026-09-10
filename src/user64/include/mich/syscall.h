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

long mich_syscall0(unsigned long number);
long mich_syscall1(unsigned long number, unsigned long arg0);
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
