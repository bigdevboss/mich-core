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
            t->id = i;
            return t;
        }
    }
    if (task_pool_count >= MAX_TASKS) return 0;
    int idx = task_pool_count++;
    struct task *t = &task_pool[idx];
    t->state = TASK_RUNNING;
    t->id = idx;
    return t;
}

void task_free_slot(struct task *t) {
    task_clear_dynamic(t);
    t->esp = 0;
    t->eip = 0;
    t->stack = 0;
    t->kernel_stack = 0;
    t->state = TASK_FREE;
}

struct task *create_task(void (*entry)(), unsigned int *stack_top, unsigned int *kernel_stack_top, int ring) {
    struct task *t = task_alloc_slot();
    if (!t) return 0;

    t->stack = stack_top;
    t->kernel_stack = (ring == 3) ? kernel_stack_top : stack_top;
    t->eip = (unsigned int)entry;
    t->ring = ring;
    t->id = (int)(t - task_pool);
    task_clear_dynamic(t);

    unsigned int *sp = t->kernel_stack;

    *--sp = 0x23;
    *--sp = (unsigned int)stack_top;
    *--sp = 0x202;
    *--sp = 0x1B;
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
