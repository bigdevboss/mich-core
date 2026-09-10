#include "scheduler.h"
#include "arch_cpu.h"
#include "task.h"
#include "arch_task.h"
#include "paging.h"
#include "proc.h"
#include "ipc.h"
#include "protos.h"

static u32 ticks;

void scheduler_init_idle(paddr_t kernel_dir_phys) {
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
    t->exec_gate = 0;
    t->recv_expect = 0;
    t->gen = 0;
    t->send_result = 0;
    t->send_to = 0;
    t->send_deadline = 0;
    t->capabilities = 0;
    t->irq_rights = 0;
    for (int m = 0; m < MAX_MMIO_GRANTS; m++) {
        t->mmio[m].phys = 0;
        t->mmio[m].length = 0;
        t->mmio[m].mapped_va = 0;
    }
    t->dma_page_limit = 0;
    t->dma_pages_used = 0;
    t->dma_max_addr = 0;
    t->page_dir = kernel_dir_phys;
    const char *n = "idle";
    int i = 0;
    while (n[i]) { t->name[i] = n[i]; i++; }
    t->name[i] = 0;
    scheduler_set_current(idx);
    ticks = 0;
}

u32 scheduler_now(void) {
    return ticks;
}

static void check_kstack_canary(struct task *t) {
    if (!t->kstack_phys) return;
    unsigned int *p = paging_kmap_a(t->kstack_phys);
    if (p[0] != KSTACK_CANARY)
        panic_str("KERNEL STACK OVERFLOW");
}

reg_t schedule_c(reg_t esp_now) {
    int current_task = scheduler_current();
    ticks++;
    ipc_tick(ticks);
    paddr_t prev_pd = 0;
    if (current_task >= 0 && current_task < task_pool_count) {
        task_pool[current_task].esp = esp_now;
        check_kstack_canary(&task_pool[current_task]);
        prev_pd = task_pool[current_task].page_dir;
    }

    proc_reap_orphans(current_task);

    int next = scheduler_pick_next(current_task);
    if (next < 0) next = 0;

    scheduler_set_current(next);
    current_task = next;
    struct task *t = &task_pool[current_task];
    check_kstack_canary(t);

    arch_task_activate(t);
    if (t->page_dir && t->page_dir != prev_pd) {
        arch_set_address_space(t->page_dir);
    }
    return t->esp;
}
