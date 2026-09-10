#ifndef SYSCALL_H
#define SYSCALL_H

#define SYS_WRITE        1
#define SYS_CLEAR        4
#define SYS_SEND         5
#define SYS_RECV         6
#define SYS_SEND_NB      7
#define SYS_IRQ_REG      8
#define SYS_IOPORT_ALLOW 9
#define SYS_TASKS        10
#define SYS_MEMFREE      11
#define SYS_FORK         12
#define SYS_EXEC         13
#define SYS_EXIT         14
#define SYS_WAIT         15
#define SYS_GETPID       16
#define SYS_KILL         17
#define SYS_SERVICE_REGISTER 18
#define SYS_SERVICE_LOOKUP   19
#define SYS_CAP_GET          20
#define SYS_CAP_DROP         21
#define SYS_CAP_GRANT        22
#define SYS_YIELD            23
#define SYS_RECV_FROM        24
#define SYS_IRQ_GRANT        25
#define SYS_IOPORT_GRANT     26
#define SYS_MMIO_GRANT       27
#define SYS_MMIO_MAP         28
#define SYS_DMA_GRANT        29
#define SYS_DMA_ALLOC        30
#define SYS_SEND_TIMEOUT     31

struct sys_task_info {
    unsigned int id;
    unsigned int state;
    unsigned int ring;
    char name[16];
};

void syscall_init(void);
int syscall_dispatcher(unsigned int eax, unsigned int ebx, unsigned int ecx, unsigned int edx);

#endif
