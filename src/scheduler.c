#include "scheduler.h"
#include "task.h"
#include "tss.h"
#include "paging.h"

static int current_task = -1;

void scheduler_init_idle(unsigned int kernel_dir_phys) {
    int idx = task_pool_count++;
    struct task *t = &task_pool[idx];
    t->esp = 0;
    t->eip = 0;
    t->stack = 0;
    t->kernel_stack = 0;
    t->ring = 0;
    t->id = idx;
    t->state = TASK_RUNNING;
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
    t->page_dir = kernel_dir_phys;
    const char *n = "idle";
    int i = 0;
    while (n[i]) { t->name[i] = n[i]; i++; }
    t->name[i] = 0;
    current_task = idx;
}

int scheduler_current(void) {
    return current_task;
}

unsigned int schedule_c(unsigned int esp_now) {
    if (current_task >= 0 && current_task < task_pool_count) {
        task_pool[current_task].esp = esp_now;
    }

    int next = current_task;
    int found = 0;
    for (int i = 0; i < task_pool_count; i++) {
        next = (next + 1) % task_pool_count;
        if (task_pool[next].state == TASK_RUNNING) {
            found = 1;
            break;
        }
    }
    if (!found) next = 0;

    current_task = next;
    struct task *t = &task_pool[current_task];

    if (t->kernel_stack) {
        tss_set_esp0((unsigned int)t->kernel_stack);
    }
    tss_load_iomap(t->iomap ? t->iomap : tss_default_iomap());
    if (t->page_dir) {
        __asm__ volatile("mov %0, %%cr3" : : "r"(t->page_dir));
    }
    return t->esp;
}
