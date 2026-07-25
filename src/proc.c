#include "task.h"
#include "scheduler.h"
#include "paging.h"
#include "pmm.h"
#include "ipc.h"
#include "protos.h"
#include "elf.h"



#define KFRAME_BYTES 52
#define EXEC_STAGE_MAX 71680
#define USER_STACK_VADDR 0x1080000

static unsigned char exec_stage[EXEC_STAGE_MAX];

static void intr_off(void) { __asm__ volatile("cli"); }
static void intr_on(void) { __asm__ volatile("sti"); }

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

static void set_name(struct task *t, const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') base = p + 1;
    int j = 0;
    while (j < 15 && base[j] && base[j] != '.') { t->name[j] = base[j]; j++; }
    t->name[j] = 0;
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
    ipc_send(FS_SERVER_ID, &m);

    unsigned int size = 0;
    for (;;) {
        ipc_recv_from(FS_SERVER_ID, &m);
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
    ipc_send(FS_SERVER_ID, &m);

    unsigned int got = 0;
    for (;;) {
        ipc_recv_from(FS_SERVER_ID, &m);
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
        unsigned char *src = (unsigned char *)paging_kmap_a(p->kstack_phys);
        unsigned char *dst = (unsigned char *)paging_kmap_b(ksp);
        for (int i = 0; i < KFRAME_BYTES; i++)
            dst[4096 - KFRAME_BYTES + i] = src[4096 - KFRAME_BYTES + i];
        dst = (unsigned char *)paging_kmap_b(ksp);
        for (int i = 0; i < 4; i++)
            dst[4096 - 24 + i] = 0;
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
    int j = 0;
    while (j < 15 && p->name[j]) { c->name[j] = p->name[j]; j++; }
    c->name[j] = 0;
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
    char path[32];
    const char *up = (const char *)user_path_ptr;
    int i = 0;
    while (i < 31 && up[i]) { path[i] = up[i]; i++; }
    path[i] = 0;
    if (!path[0]) return -EINVAL;

    char args[200];
    const char *ua = (const char *)user_args_ptr;
    int a = 0;
    if (ua) {
        while (a < 199 && ua[a]) { args[a] = ua[a]; a++; }
    }
    args[a] = 0;

    unsigned int size = 0;
    struct task *t = &task_pool[scheduler_current()];
    t->exec_gate = 1;
    int fr = fs_fetch(path, exec_stage, &size);
    t->exec_gate = 0;
    if (fr) return fr;

    intr_off();
    unsigned int new_pd = pmm_alloc_page_low();
    if (!new_pd) { intr_on(); return -ENOMEM; }
    paging_create_user_directory(new_pd);

    unsigned int entry = 0;
    if (elf_map(new_pd, exec_stage, size, &entry) != 0) {
        paging_free_user_pages(new_pd);
        pmm_free_page(new_pd);
        intr_on();
        return -EINVAL;
    }

    unsigned int ustack = pmm_alloc_page();
    if (!ustack) {
        paging_free_user_pages(new_pd);
        pmm_free_page(new_pd);
        intr_on();
        return -ENOMEM;
    }
    paging_map_user(new_pd, ustack, USER_STACK_VADDR, 0x6);
    {
        unsigned char *w = (unsigned char *)paging_kmap_b(ustack);
        for (int b = 0; b < 4096; b++) w[b] = 0;
        int j = 0;
        while (j <= a) { w[3584 + j] = (unsigned char)args[j]; j++; }
    }

    unsigned int old_pd = t->page_dir;
    __asm__ volatile("mov %0, %%cr3" : : "r"(new_pd));
    paging_free_user_pages(old_pd);
    pmm_free_page(old_pd);
    t->page_dir = new_pd;

    unsigned char *kf = (unsigned char *)paging_kmap_a(t->kstack_phys);
    for (int b = 0; b < 32; b++)
        kf[4096 - KFRAME_BYTES + b] = 0;
    unsigned int *fr32 = (unsigned int *)(kf + 4096 - KFRAME_BYTES);
    fr32[4] = (ua && args[0]) ? USER_STACK_VADDR + 3584 : 0;
    fr32[8] = entry;
    fr32[9] = 0x1B;
    fr32[10] = 0x202;
    fr32[11] = USER_STACK_VADDR + 4096;
    fr32[12] = 0x23;
    intr_on();

    set_name(t, path);
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

    if (pid <= 0 || pid >= MAX_TASKS || pid == me->id) return -1;

    struct task *slot = &task_pool[pid];
    if (slot->state == TASK_FREE || slot->parent_id != me->id) return -1;

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
    if (pid <= INIT_PID) return -EPERM;
    if (pid >= MAX_TASKS) return -ESRCH;
    struct task *t = &task_pool[pid];
    if (t->state == TASK_FREE) return -ESRCH;
    if (pid == me->id) return proc_exit(137);

    int p = me->parent_id;
    while (p > 0) {
        if (p == pid) return -EPERM;
        p = task_pool[p].parent_id;
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
