#ifndef MICH_USER_SYSCALL_H
#define MICH_USER_SYSCALL_H

#define MICH_SYS_WRITE   1
#define MICH_SYS_CLEAR   4
#define MICH_SYS_SEND    5
#define MICH_SYS_RECV    6
#define MICH_SYS_SEND_NB 7
#define MICH_SYS_TASKS   10
#define MICH_SYS_MEMFREE 11
#define MICH_SYS_FORK    12
#define MICH_SYS_EXEC    13
#define MICH_SYS_EXIT    14
#define MICH_SYS_WAIT    15
#define MICH_SYS_GETPID  16
#define MICH_SYS_KILL    17
#define MICH_SYS_SERVICE_REGISTER 18
#define MICH_SYS_SERVICE_LOOKUP   19
#define MICH_SYS_CAP_GET          20
#define MICH_SYS_CAP_DROP         21
#define MICH_SYS_CAP_GRANT        22
#define MICH_SYS_YIELD            23
#define MICH_SYS_RECV_FROM        24
#define MICH_SYS_IRQ_GRANT        25
#define MICH_SYS_IOPORT_GRANT     26
#define MICH_SYS_MMIO_GRANT       27
#define MICH_SYS_MMIO_MAP         28
#define MICH_SYS_DMA_GRANT        29
#define MICH_SYS_DMA_ALLOC        30
#define MICH_SYS_SEND_TIMEOUT     31

int mich_write(const char *text);
int mich_clear(void);
int mich_memfree(void);
int mich_fork(void);
int mich_exec(const char *path, const char *args);
void mich_exit(int code) __attribute__((noreturn));
int mich_wait(int pid);
int mich_getpid(void);
int mich_kill(int pid);
int mich_yield(void);

#endif
