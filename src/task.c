#include "task.h"

struct task task_pool[MAX_TASKS];
int task_pool_count = 0;

static void task_clear_dynamic(struct task *t) {
    t->page_dir = 0;
    t->recv_buf = 0;
    t->pending_msg = 0;
    t->iomap = 0;
    t->next = 0;
    t->wq_head = 0;
    t->wq_tail = 0;
    t->parent_id = -1;
    t->exit_code = 0;
    t->wait_pid = -1;
    t->kstack_phys = 0;
    t->exec_gate = 0;
    t->recv_expect = 0;
    t->name[0] = 0;
}

struct task *task_alloc_slot(void) {
    for (int i = 1; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_FREE) {
            struct task *t = &task_pool[i];
            t->state = TASK_RUNNING;
            t->id = t->gen ? (int)PID_MAKE(t->gen, (unsigned int)i) : i;
            return t;
        }
    }
    if (task_pool_count >= MAX_TASKS) return 0;
    int idx = task_pool_count++;
    struct task *t = &task_pool[idx];
    t->state = TASK_RUNNING;
    t->id = t->gen ? (int)PID_MAKE(t->gen, (unsigned int)idx) : idx;
    return t;
}

void task_free_slot(struct task *t) {
    unsigned int g = t->gen;
    task_clear_dynamic(t);
    t->esp = 0;
    t->eip = 0;
    t->stack = 0;
    t->kernel_stack = 0;
    t->gen = g + 1;
    t->state = TASK_FREE;
}

void task_set_name(struct task *t, const char *path) {
    const char *base = path;
    if (!base || !base[0]) base = "elf";
    for (const char *p = base; *p; p++)
        if (*p == '/') base = p + 1;
    int j = 0;
    while (j < 15 && base[j] && base[j] != '.') { t->name[j] = base[j]; j++; }
    t->name[j] = 0;
}

struct task *create_task(void (*entry)(), unsigned int *stack_top, unsigned int *kernel_stack_top, int ring) {
    struct task *t = task_alloc_slot();
    if (!t) return 0;

    t->stack = stack_top;
    t->kernel_stack = (ring == 3) ? kernel_stack_top : stack_top;
    t->eip = (unsigned int)entry;
    t->ring = ring;
    task_clear_dynamic(t);

    unsigned int *sp = t->kernel_stack;

    *--sp = USER_SS;
    *--sp = (unsigned int)stack_top;
    *--sp = USER_EFLAGS;
    *--sp = USER_CS;
    *--sp = t->eip;

    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;

    t->esp = (unsigned int)sp;
    return t;
}
