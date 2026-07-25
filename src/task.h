#ifndef TASK_H
#define TASK_H

#define MAX_TASKS 16

#define TASK_RUNNING 0
#define TASK_BLOCKED_SEND 1
#define TASK_BLOCKED_RECV 2
#define TASK_ZOMBIE 3
#define TASK_FREE 4
#define TASK_BLOCKED_WAIT 5

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
};

extern struct task task_pool[MAX_TASKS];
extern int task_pool_count;

struct task *create_task(void (*entry)(), unsigned int *stack_top, unsigned int *kernel_stack_top, int ring);
struct task *task_alloc_slot(void);
void task_free_slot(struct task *t);

#endif
