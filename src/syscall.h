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

struct sys_task_info {
    unsigned int id;
    unsigned int state;
    unsigned int ring;
    char name[16];
};

void syscall_init(void);
int syscall_dispatcher(unsigned int eax, unsigned int ebx, unsigned int ecx, unsigned int edx);

#endif
