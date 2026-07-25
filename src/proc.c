#include "task.h"
#include "scheduler.h"
#include "paging.h"
#include "pmm.h"
#include "ipc.h"
#include "irq.h"
#include "protos.h"
#include "elf.h"
#include "uaccess.h"

#define EXEC_STAGE_MAX 71680
#define EXEC_ARGS_OFF 3584
#define USER_STACK_VADDR 0x1080000

static unsigned char exec_stage[EXEC_STAGE_MAX];
static volatile unsigned int exec_lock = 0;

static unsigned int rd32p(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void wr32p(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void exec_lock_take(void) {
    for (;;) {
        intr_off();
        if (!exec_lock) {
            exec_lock = 1;
            intr_on();
            return;
        }
        intr_on();
        __asm__ volatile("sti; hlt");
    }
}

static void exec_lock_give(void) {
    intr_off();
    exec_lock = 0;
    intr_on();
}

static void fs_put_name(struct message *m, const char *name) {
    for (int i = 0; i < 56; i++) m->data[i] = 0;
    int i = 0;
    while (name[i] && i < 27) { m->data[8 + i] = (unsigned char)name[i]; i++; }
}

static int fs_fetch(const char *path, unsigned char *buf, unsigned int *size_out) {
    struct message m;
    m.type = FS_STAT;
    m.from_id = 0;
    fs_put_name(&m, path);
    if (ipc_send(FS_SERVER_ID, &m) != 0) return -ESRCH;

    unsigned int size = 0;
    for (;;) {
        ipc_recv_from(FS_SERVER_ID, &m);
        if (m.type == MSG_DIED) return -ESRCH;
        if (m.type == FS_ERR) return -(int)rd32p(m.data);
        if (m.type == FS_STAT_RSP) {
            if (m.data[4] != 1) return -EISDIR;
            size = rd32p(m.data);
            break;
        }
    }
    if (size == 0 || size > EXEC_STAGE_MAX) return -EINVAL;

    m.type = FS_READ;
    m.from_id = 0;
    for (int i = 0; i < 56; i++) m.data[i] = 0;
    wr32p(m.data, 0);
    wr32p(m.data + 4, size);
    int i = 0;
    while (path[i] && i < 27) { m.data[8 + i] = (unsigned char)path[i]; i++; }
    if (ipc_send(FS_SERVER_ID, &m) != 0) return -ESRCH;

    unsigned int got = 0;
    for (;;) {
        ipc_recv_from(FS_SERVER_ID, &m);
        if (m.type == MSG_DIED) return -ESRCH;
        if (m.type == FS_ERR) return -(int)rd32p(m.data);
        if (m.type != FS_DATA) continue;
        unsigned int n = rd32p(m.data);
        if (n > FS_PAYLOAD) n = FS_PAYLOAD;
        if (got + n > EXEC_STAGE_MAX) return -EINVAL;
        for (unsigned int j = 0; j < n; j++) buf[got + j] = m.data[4 + j];
        got += n;
        if (got >= size) break;
    }
    *size_out = size;
    return 0;
}

int proc_fork(void) {
    struct task *p = &task_pool[scheduler_current()];
    struct task *c = task_alloc_slot();
    if (!c) return -1;

    intr_off();
    int rc = -1;
    unsigned int new_pd = 0;
    unsigned int ksp = 0;

    new_pd = pmm_alloc_page_low();
    if (!new_pd) goto out;
    paging_create_user_directory(new_pd);
    ksp = pmm_alloc_page_low();
    if (!ksp) goto out;

    if (!paging_clone_user(new_pd, p->page_dir)) goto out;

    {
        unsigned int *s32 = paging_kmap_a(p->kstack_phys) + (4096 - KFRAME_BYTES) / 4;
        unsigned int *d32 = paging_kmap_b(ksp) + (4096 - KFRAME_BYTES) / 4;
        for (int i = 0; i < KFRAME_BYTES / 4; i++) d32[i] = s32[i];
        d32[KFR_EAX] = 0;
    }

    c->stack = p->stack;
    c->kernel_stack = (unsigned int *)(ksp + 4096);
    c->kstack_phys = ksp;
    c->esp = ksp + 4096 - KFRAME_BYTES;
    c->eip = p->eip;
    c->ring = 3;
    c->page_dir = new_pd;
    c->parent_id = p->id;
    c->iomap = p->iomap;
    task_set_name(c, p->name);
    rc = c->id;

out:
    if (rc < 0) {
        if (new_pd) {
            paging_free_user_pages(new_pd);
            pmm_free_page(new_pd);
        }
        if (ksp) pmm_free_page(ksp);
        task_free_slot(c);
    }
    intr_on();
    return rc;
}

int proc_exec(unsigned int user_path_ptr, unsigned int user_args_ptr) {
    unsigned int me_pd = task_pool[scheduler_current()].page_dir;
    char path[32];
    char args[200];

    intr_off();
    int pn = ua_copy_str(me_pd, path, user_path_ptr, sizeof(path));
    int an = 0;
    if (user_args_ptr)
        an = ua_copy_str(me_pd, args, user_args_ptr, sizeof(args));
    intr_on();
    if (pn <= 0) return -EINVAL;
    if (an < 0) return -EINVAL;
    args[an] = 0;

    unsigned int size = 0;
    struct task *t = &task_pool[scheduler_current()];
    exec_lock_take();
    t->exec_gate = 1;
    int fr = fs_fetch(path, exec_stage, &size);
    t->exec_gate = 0;
    if (fr) {
        exec_lock_give();
        return fr;
    }

    intr_off();
    unsigned int new_pd = pmm_alloc_page_low();
    if (!new_pd) { intr_on(); exec_lock_give(); return -ENOMEM; }
    paging_create_user_directory(new_pd);

    unsigned int entry = 0;
    if (elf_map(new_pd, exec_stage, size, &entry) != 0) {
        paging_free_user_pages(new_pd);
        pmm_free_page(new_pd);
        intr_on();
        exec_lock_give();
        return -EINVAL;
    }
    exec_lock_give();

    unsigned int ustack = pmm_alloc_page();
    if (!ustack) {
        paging_free_user_pages(new_pd);
        pmm_free_page(new_pd);
        intr_on();
        return -ENOMEM;
    }
    paging_map_user(new_pd, ustack, USER_STACK_VADDR, USER_FLAGS);
    {
        unsigned char *w = (unsigned char *)paging_kmap_b(ustack);
        for (int b = 0; b < 4096; b++) w[b] = 0;
        int j = 0;
        while (j <= an) { w[EXEC_ARGS_OFF + j] = (unsigned char)args[j]; j++; }
    }

    unsigned int old_pd = t->page_dir;
    __asm__ volatile("mov %0, %%cr3" : : "r"(new_pd));
    paging_free_user_pages(old_pd);
    pmm_free_page(old_pd);
    t->page_dir = new_pd;

    unsigned int *fr32 = paging_kmap_a(t->kstack_phys) + (4096 - KFRAME_BYTES) / 4;
    for (int i = 0; i < KFR_EIP; i++) fr32[i] = 0;
    fr32[KFR_EBX] = (user_args_ptr && args[0]) ? USER_STACK_VADDR + EXEC_ARGS_OFF : 0;
    fr32[KFR_EIP] = entry;
    fr32[KFR_CS] = USER_CS;
    fr32[KFR_EFLAGS] = USER_EFLAGS;
    fr32[KFR_UESP] = USER_STACK_VADDR + 4096;
    fr32[KFR_USS] = USER_SS;
    intr_on();

    task_set_name(t, path);
    return 0;
}

int proc_exit(int code) {
    struct task *t = &task_pool[scheduler_current()];
    if (t->id == 0) return -1;

    ipc_flush_task((unsigned int)t->id);
    intr_off();
    __asm__ volatile("mov %0, %%cr3" : : "r"(paging_kernel_dir_phys()));
    paging_free_user_pages(t->page_dir);
    t->exit_code = code;
    t->state = TASK_ZOMBIE;
    for (int i = 0; i < task_pool_count; i++) {
        struct task *w = &task_pool[i];
        if (w->state == TASK_BLOCKED_WAIT && (w->wait_pid == t->id || w->wait_pid == -1))
            w->state = TASK_RUNNING;
    }
    intr_on();

    for (;;) __asm__ volatile("sti; hlt");
}

static int reap(struct task *slot) {
    int code = slot->exit_code;
    if (slot->page_dir) pmm_free_page(slot->page_dir);
    if (slot->kstack_phys) pmm_free_page(slot->kstack_phys);
    task_free_slot(slot);
    return code;
}

static int find_child(int me_id, int zombie_only) {
    for (int i = 1; i < task_pool_count; i++) {
        struct task *s = &task_pool[i];
        if (s->parent_id == me_id && s->state != TASK_FREE) {
            if (!zombie_only || s->state == TASK_ZOMBIE) return i;
        }
    }
    return -1;
}

int proc_wait(int pid) {
    struct task *me = &task_pool[scheduler_current()];

    if (pid == -2) {
        int z = find_child(me->id, 1);
        if (z < 0) return -1;
        struct task *slot = &task_pool[z];
        int id = slot->id;
        int code = reap(slot);
        return (id << 16) | (code & 0xFFFF);
    }

    if (pid == -1) {
        for (;;) {
            int z = find_child(me->id, 1);
            if (z >= 0) return reap(&task_pool[z]);
            if (find_child(me->id, 0) < 0) return -1;
            me->wait_pid = -1;
            me->state = TASK_BLOCKED_WAIT;
            while (me->state == TASK_BLOCKED_WAIT)
                __asm__ volatile("sti; hlt");
            me->wait_pid = -1;
        }
    }

    unsigned int slot_idx = PID_SLOT((unsigned int)pid);
    if (slot_idx == 0 || slot_idx >= (unsigned int)task_pool_count) return -1;

    struct task *slot = &task_pool[slot_idx];
    if (slot->state == TASK_FREE || (unsigned int)slot->id != (unsigned int)pid) return -1;
    if (slot->parent_id != me->id) return -1;

    if (slot->state != TASK_ZOMBIE) {
        me->wait_pid = pid;
        me->state = TASK_BLOCKED_WAIT;
        while (me->state == TASK_BLOCKED_WAIT)
            __asm__ volatile("sti; hlt");
        me->wait_pid = -1;
    }

    if (slot->state == TASK_ZOMBIE) return reap(slot);
    return -1;
}

int proc_getpid(void) {
    return task_pool[scheduler_current()].id;
}

int proc_kill(int pid) {
    struct task *me = &task_pool[scheduler_current()];
    unsigned int slot_idx = PID_SLOT((unsigned int)pid);
    if (slot_idx == 0 || slot_idx >= (unsigned int)task_pool_count) return -ESRCH;
    if (PID_GEN((unsigned int)pid) == 0 && (int)slot_idx <= INIT_PID) return -EPERM;

    struct task *t = &task_pool[slot_idx];
    if (t->state == TASK_FREE || (unsigned int)t->id != (unsigned int)pid) return -ESRCH;
    if ((unsigned int)me->id == (unsigned int)pid) return proc_exit(137);

    int p = me->parent_id;
    while (p > 0) {
        unsigned int ps = PID_SLOT((unsigned int)p);
        if (ps == 0 || ps >= (unsigned int)task_pool_count) break;
        if (p == pid) return -EPERM;
        p = task_pool[ps].parent_id;
    }

    if (t->state == TASK_ZOMBIE) return 0;

    intr_off();
    for (int i = 0; i < task_pool_count; i++) {
        struct task *d = &task_pool[i];
        struct task *prev = 0;
        struct task *w = d->wq_head;
        while (w) {
            if (w == t) {
                if (prev) prev->next = w->next;
                else d->wq_head = w->next;
                if (d->wq_tail == t) d->wq_tail = prev;
                break;
            }
            prev = w;
            w = w->next;
        }
    }
    intr_on();
    ipc_flush_task((unsigned int)pid);
    intr_off();
    paging_free_user_pages(t->page_dir);
    t->exit_code = 137;
    t->state = TASK_ZOMBIE;
    t->wait_pid = -1;
    for (int i = 0; i < task_pool_count; i++) {
        struct task *w = &task_pool[i];
        if (w->state == TASK_BLOCKED_WAIT && (w->wait_pid == t->id || w->wait_pid == -1))
            w->state = TASK_RUNNING;
    }
    intr_on();
    return 0;
}
