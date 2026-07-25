#ifndef TASK_H
#define TASK_H

#define MAX_TASKS 16

#define TASK_RUNNING 0
#define TASK_BLOCKED_SEND 1
#define TASK_BLOCKED_RECV 2
#define TASK_ZOMBIE 3
#define TASK_FREE 4
#define TASK_BLOCKED_WAIT 5

#define USER_CS 0x1B
#define USER_SS 0x23
#define USER_EFLAGS 0x202

#define KFRAME_BYTES 52
#define KFR_EDI 0
#define KFR_ESI 1
#define KFR_EBP 2
#define KFR_ESP 3
#define KFR_EBX 4
#define KFR_EDX 5
#define KFR_ECX 6
#define KFR_EAX 7
#define KFR_EIP 8
#define KFR_CS 9
#define KFR_EFLAGS 10
#define KFR_UESP 11
#define KFR_USS 12

#define PID_SLOT(pid) ((unsigned int)(pid) & 0xFFFF)
#define PID_GEN(pid)  ((unsigned int)(pid) >> 16)
#define PID_MAKE(gen, slot) ((((unsigned int)(gen)) << 16) | ((unsigned int)(slot) & 0xFFFF))

struct message;

struct task {
    unsigned int esp;
    unsigned int eip;
    unsigned int page_dir;
    unsigned int *stack;
    unsigned int *kernel_stack;
    int id;
    int ring;
    int state;
    struct message *recv_buf;
    const struct message *pending_msg;
    const unsigned char *iomap;
    char name[16];
    struct task *next;
    struct task *wq_head;
    struct task *wq_tail;
    int parent_id;
    int exit_code;
    int wait_pid;
    unsigned int kstack_phys;
    unsigned int exec_gate;
    unsigned int recv_expect;
    unsigned int gen;
};

extern struct task task_pool[MAX_TASKS];
extern int task_pool_count;

static inline unsigned int task_full_pid(const struct task *t) {
    return PID_MAKE(t->gen, (unsigned int)(t - task_pool));
}

struct task *create_task(void (*entry)(), unsigned int *stack_top, unsigned int *kernel_stack_top, int ring);
struct task *task_alloc_slot(void);
void task_free_slot(struct task *t);
void task_set_name(struct task *t, const char *path);

#endif
