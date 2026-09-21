#include "task.h"
#include "arch_task.h"
#ifdef __x86_64__
#include "posix_fd.h"
#include "posix_profile.h"
#endif

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
    t->send_result = 0;
    t->send_to = 0;
    t->send_deadline = 0;
    t->capabilities = 0;
    t->irq_rights = 0;
    for (int i = 0; i < MAX_MMIO_GRANTS; i++) {
        t->mmio[i].phys = 0;
        t->mmio[i].length = 0;
        t->mmio[i].mapped_va = 0;
    }
    t->dma_page_limit = 0;
    t->dma_pages_used = 0;
    t->dma_max_addr = 0;
    t->on_cpu = TASK_CPU_NONE;
    t->name[0] = 0;
}

struct task *task_alloc_slot(void) {
    for (int i = 1; i < MAX_TASKS; i++) {
        if (task_pool[i].state == TASK_FREE) {
            struct task *t = &task_pool[i];
            t->state = TASK_RUNNING;
            t->on_cpu = TASK_CPU_NONE;
            t->id = t->gen ? (int)PID_MAKE(t->gen, (unsigned int)i) : i;
            return t;
        }
    }
    if (task_pool_count >= MAX_TASKS) return 0;
    int idx = task_pool_count++;
    struct task *t = &task_pool[idx];
    t->state = TASK_RUNNING;
    t->on_cpu = TASK_CPU_NONE;
    t->id = t->gen ? (int)PID_MAKE(t->gen, (unsigned int)idx) : idx;
    return t;
}

void task_free_slot(struct task *t) {
    unsigned int g = t->gen;
#ifdef __x86_64__
    posix_fd_close_all(t);
    posix_profile_release(t);
#endif
    arch_task_release(t);
    task_clear_dynamic(t);
    t->esp = 0;
    t->eip = 0;
    t->stack = 0;
    t->kernel_stack = 0;
    g = (g + 1) & 0x7FFFu;
    if (!g) g = 1;
    t->gen = g;
    t->state = TASK_FREE;
}

void task_mark_zombie(struct task *t, int code) {
#ifdef __x86_64__
    posix_fd_close_all(t);
    posix_profile_release(t);
#endif
    t->exit_code = code;
    t->on_cpu = TASK_CPU_NONE;
    t->state = TASK_ZOMBIE;
    t->recv_buf = 0;
    t->recv_expect = 0;
    t->pending_msg = 0;
    t->send_to = 0;
    t->send_deadline = 0;
    t->next = 0;
    t->wait_pid = -1;
    t->capabilities = 0;
    t->irq_rights = 0;
}

int task_reap_zombie(struct task *parent, int pid, int *code) {
    unsigned int slot = PID_SLOT((unsigned int)pid);
    if (!parent || !code || slot == 0 || slot >= (unsigned int)task_pool_count)
        return -1;
    struct task *child = &task_pool[slot];
    if (child->state != TASK_ZOMBIE || child->id != pid ||
        child->parent_id != parent->id)
        return -1;
    *code = child->exit_code;
    task_free_slot(child);
    return 0;
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

struct task *create_task(void (*entry)(void), reg_t *stack_top, reg_t *kernel_stack_top, int ring) {
    struct task *t = task_alloc_slot();
    if (!t) return 0;

    t->stack = stack_top;
    t->kernel_stack = (ring == 3) ? kernel_stack_top : stack_top;
    t->eip = (reg_t)(uptr_t)entry;
    t->ring = ring;
    task_clear_dynamic(t);

    reg_t *sp = t->kernel_stack;

    *--sp = USER_SS;
    *--sp = (reg_t)(uptr_t)stack_top;
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

    t->esp = (reg_t)(uptr_t)sp;
    return t;
}
