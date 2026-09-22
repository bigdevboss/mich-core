#ifndef TASK_H
#define TASK_H

#include "types.h"

#define MAX_TASKS 16
#define MAX_MMIO_GRANTS 4

struct mmio_grant {
    paddr_t phys;
    usize_t length;
    vaddr_t mapped_va;
};

#define TASK_RUNNING 0
#define TASK_BLOCKED_SEND 1
#define TASK_BLOCKED_RECV 2
#define TASK_ZOMBIE 3
#define TASK_FREE 4
#define TASK_BLOCKED_WAIT 5
#define TASK_BLOCKED_EVENT 6

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

#define KSTACK_CANARY 0x4D494348U
#define TASK_CPU_NONE (-1)

struct message;

struct task {
    reg_t esp;
    reg_t eip;
    paddr_t page_dir;
    reg_t *stack;
    reg_t *kernel_stack;
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
    // POSIX waitpid waiter state: the blocking dispatch frame is abandoned
    // on switch, so the wake path itself must deliver the reaped pid and
    // publish the encoded status word into the waiter's memory.
    int wait_posix;
    uptr_t wait_status_address;
    paddr_t kstack_phys;
    u32 exec_gate;
    u32 recv_expect;
    u32 gen;
    int send_result;
    u32 send_to;
    u32 send_deadline;
    u32 capabilities;
    u32 irq_rights;
    struct mmio_grant mmio[MAX_MMIO_GRANTS];
    u32 dma_page_limit;
    u32 dma_pages_used;
    paddr_t dma_max_addr;
    // CPU currently executing this task, or TASK_CPU_NONE. pick_next
    // skips a task owned by another CPU so two cores cannot run it.
    int on_cpu;
};

extern struct task task_pool[MAX_TASKS];
extern int task_pool_count;

static inline unsigned int task_full_pid(const struct task *t) {
    return PID_MAKE(t->gen, (unsigned int)(t - task_pool));
}

struct task *create_task(void (*entry)(void), reg_t *stack_top, reg_t *kernel_stack_top, int ring);
struct task *task_alloc_slot(void);
void task_free_slot(struct task *t);
void task_mark_zombie(struct task *t, int code);
int task_reap_zombie(struct task *parent, int pid, int *code);
void task_set_name(struct task *t, const char *path);

#endif
