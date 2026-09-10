#include "task.h"
#include "arch_cpu.h"
#include "capability.h"
#include "scheduler.h"
#include "paging.h"
#include "pmm.h"
#include "ipc.h"
#include "irq.h"
#include "protos.h"
#include "elf.h"
#include "uaccess.h"
#include "arch_task.h"
#include "serial.h"
#include "service.h"
#include "object.h"
#include "event.h"

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
        irq_state_t state = irq_save();
        if (!exec_lock) {
            exec_lock = 1;
            irq_restore(state);
            return;
        }
        irq_restore(state);
        if (state & IRQ_STATE_IF)
            arch_cpu_halt();
        else
            arch_cpu_pause();
    }
}

static void exec_lock_give(void) {
    irq_state_t state = irq_save();
    exec_lock = 0;
    irq_restore(state);
}

static void fs_put_name(struct message *m, const char *name) {
    for (int i = 0; i < 56; i++) m->data[i] = 0;
    int i = 0;
    while (name[i] && i < 27) { m->data[8 + i] = (unsigned char)name[i]; i++; }
}

static int fs_fetch(const char *path, unsigned char *buf, unsigned int *size_out) {
    int fs_pid = service_lookup(SERVICE_FS);
    if (fs_pid < 0) return ESRCH;

    struct message m;
    m.type = FS_STAT;
    m.from_id = 0;
    fs_put_name(&m, path);
    if (ipc_send((unsigned int)fs_pid, &m) != 0) return ESRCH;

    unsigned int size = 0;
    for (;;) {
        ipc_recv_from((unsigned int)fs_pid, &m);
        if (m.type == MSG_DIED) return ESRCH;
        if (m.type == FS_ERR) return -(int)rd32p(m.data);
        if (m.type == FS_STAT_RSP) {
            if (m.data[4] != 1) return EISDIR;
            size = rd32p(m.data);
            break;
        }
    }
    if (size == 0 || size > EXEC_STAGE_MAX) return EINVAL;

    m.type = FS_READ;
    m.from_id = 0;
    for (int i = 0; i < 56; i++) m.data[i] = 0;
    wr32p(m.data, 0);
    wr32p(m.data + 4, size);
    int i = 0;
    while (path[i] && i < 27) { m.data[8 + i] = (unsigned char)path[i]; i++; }
    if (ipc_send((unsigned int)fs_pid, &m) != 0) return ESRCH;

    unsigned int got = 0;
    for (;;) {
        ipc_recv_from((unsigned int)fs_pid, &m);
        if (m.type == MSG_DIED) return ESRCH;
        if (m.type == FS_ERR) return -(int)rd32p(m.data);
        if (m.type != FS_DATA) continue;
        unsigned int n = rd32p(m.data);
        if (n > FS_PAYLOAD) n = FS_PAYLOAD;
        if (got + n > EXEC_STAGE_MAX) return EINVAL;
        for (unsigned int j = 0; j < n; j++) buf[got + j] = m.data[4 + j];
        got += n;
        if (got >= size) break;
    }
    *size_out = size;
    return 0;
}

int proc_fork(void) {
    irq_state_t state = irq_save();
    struct task *p = &task_pool[scheduler_current()];
    struct task *c = task_alloc_slot();
    if (!c) {
        irq_restore(state);
        return -1;
    }

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
        d32 = paging_kmap_b(ksp);
        d32[0] = KSTACK_CANARY;
    }

    c->stack = p->stack;
    c->kernel_stack = (reg_t *)(uptr_t)(ksp + 4096);
    c->kstack_phys = ksp;
    c->esp = ksp + 4096 - KFRAME_BYTES;
    c->eip = p->eip;
    c->ring = 3;
    c->page_dir = new_pd;
    c->parent_id = p->id;
    c->iomap = 0;
    c->capabilities = 0;
    c->irq_rights = 0;
    for (int m = 0; m < MAX_MMIO_GRANTS; m++) {
        c->mmio[m].phys = 0;
        c->mmio[m].length = 0;
        c->mmio[m].mapped_va = 0;
    }
    c->dma_page_limit = 0;
    c->dma_pages_used = 0;
    c->dma_max_addr = 0;
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
    irq_restore(state);
    return rc;
}

int proc_exec(unsigned int user_path_ptr, unsigned int user_args_ptr) {
    unsigned int me_pd = task_pool[scheduler_current()].page_dir;
    char path[32];
    char args[200];

    irq_state_t copy_state = irq_save();
    int pn = ua_copy_str(me_pd, path, user_path_ptr, sizeof(path));
    int an = 0;
    if (user_args_ptr)
        an = ua_copy_str(me_pd, args, user_args_ptr, sizeof(args));
    irq_restore(copy_state);
    if (pn <= 0) return EINVAL;
    if (an < 0) return EINVAL;
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

    irq_state_t irq_state = irq_save();
    unsigned int new_pd = pmm_alloc_page_low();
    if (!new_pd) {
        irq_restore(irq_state);
        exec_lock_give();
        return ENOMEM;
    }
    paging_create_user_directory(new_pd);

    unsigned int entry = 0;
    int erc = elf_map(new_pd, exec_stage, size, &entry);
    if (erc != 0) {
        serial_write("exec: elf_map rc=");
        serial_write_hex((unsigned int)erc);
        serial_write("\n");
        paging_free_user_pages(new_pd);
        pmm_free_page(new_pd);
        irq_restore(irq_state);
        exec_lock_give();
        return EINVAL;
    }
    exec_lock_give();

    unsigned int ustack = pmm_alloc_page();
    if (!ustack) {
        paging_free_user_pages(new_pd);
        pmm_free_page(new_pd);
        irq_restore(irq_state);
        return ENOMEM;
    }
    if (paging_map_user(new_pd, ustack, USER_STACK_VADDR, USER_FLAGS) != 0) {
        pmm_free_page(ustack);
        paging_free_user_pages(new_pd);
        pmm_free_page(new_pd);
        irq_restore(irq_state);
        return ENOMEM;
    }
    {
        unsigned char *w = (unsigned char *)paging_kmap_b(ustack);
        for (int b = 0; b < 4096; b++) w[b] = 0;
        int j = 0;
        while (j <= an) { w[EXEC_ARGS_OFF + j] = (unsigned char)args[j]; j++; }
    }

    unsigned int old_pd = t->page_dir;
    arch_set_address_space(new_pd);
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
    arch_task_release(t);
    for (int m = 0; m < MAX_MMIO_GRANTS; m++) t->mmio[m].mapped_va = 0;
    t->dma_pages_used = 0;
    irq_restore(irq_state);

    task_set_name(t, path);
    return 0;
}

static void reparent_children(int dead_id) {
    int new_parent = service_lookup(SERVICE_INIT);
    if (new_parent == dead_id || new_parent < 0) new_parent = 0;

    for (int i = 1; i < task_pool_count; i++) {
        struct task *child = &task_pool[i];
        if (child->state == TASK_FREE || child->parent_id != dead_id) continue;
        child->parent_id = new_parent;

        if (new_parent > 0 && child->state == TASK_ZOMBIE) {
            unsigned int ps = PID_SLOT((unsigned int)new_parent);
            if (ps < (unsigned int)task_pool_count) {
                struct task *adopter = &task_pool[ps];
                if (adopter->id == new_parent &&
                    adopter->state == TASK_BLOCKED_WAIT &&
                    (adopter->wait_pid == -1 || adopter->wait_pid == child->id))
                    adopter->state = TASK_RUNNING;
            }
        }
    }
}

static void wake_waiters(int pid) {
    for (int i = 0; i < task_pool_count; i++) {
        struct task *waiter = &task_pool[i];
        if (waiter->state == TASK_BLOCKED_WAIT &&
            (waiter->wait_pid == pid || waiter->wait_pid == -1))
            waiter->state = TASK_RUNNING;
    }
}

static void terminate_task(struct task *task, int code, int current) {
    int pid = task->id;
    ipc_flush_task((u32)pid);
    event_cancel_task((u32)(task - task_pool), ESRCH);
    handle_close_all(task);
    if (current) arch_set_address_space(paging_kernel_dir_phys());
    paging_free_user_pages(task->page_dir);
    arch_task_release(task);
    task->exit_code = code;
    task->state = TASK_ZOMBIE;
    task->recv_buf = 0;
    task->recv_expect = 0;
    task->pending_msg = 0;
    task->send_to = 0;
    task->send_deadline = 0;
    task->next = 0;
    task->wait_pid = -1;
    task->capabilities = 0;
    task->irq_rights = 0;
    reparent_children(pid);
    irq_release_owner((u32)pid);
    service_release_owner((u32)pid);
    wake_waiters(pid);
}

int proc_exit(int code) {
    struct task *task = &task_pool[scheduler_current()];
    if (task->id == 0) return -1;
    irq_state_t state = irq_save();
    terminate_task(task, code, 1);
    irq_restore(state);
    for (;;) arch_cpu_wait();
}

static int reap(struct task *slot) {
    int code = slot->exit_code;
    if (slot->page_dir) pmm_free_page(slot->page_dir);
    if (slot->kstack_phys) pmm_free_page(slot->kstack_phys);
    task_free_slot(slot);
    return code;
}

void proc_reap_orphans(int current_slot) {
    for (int i = 1; i < task_pool_count; i++) {
        struct task *slot = &task_pool[i];
        if (i == current_slot || slot->state != TASK_ZOMBIE || slot->parent_id > 0)
            continue;
        reap(slot);
    }
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
        irq_state_t state = irq_save();
        int z = find_child(me->id, 1);
        if (z < 0) {
            irq_restore(state);
            return -1;
        }
        struct task *slot = &task_pool[z];
        int id = slot->id;
        int code = reap(slot);
        irq_restore(state);
        return (id << 16) | (code & 0xFFFF);
    }

    if (pid == -1) {
        for (;;) {
            irq_state_t state = irq_save();
            int z = find_child(me->id, 1);
            if (z >= 0) {
                int code = reap(&task_pool[z]);
                irq_restore(state);
                return code;
            }
            if (find_child(me->id, 0) < 0) {
                irq_restore(state);
                return -1;
            }
            me->wait_pid = -1;
            me->state = TASK_BLOCKED_WAIT;
            irq_restore(state);
            while (me->state == TASK_BLOCKED_WAIT) arch_cpu_wait();
        }
    }

    unsigned int slot_idx = PID_SLOT((unsigned int)pid);
    if (slot_idx == 0 || slot_idx >= (unsigned int)task_pool_count) return -1;

    for (;;) {
        irq_state_t state = irq_save();
        struct task *slot = &task_pool[slot_idx];
        if (slot->state == TASK_FREE || (unsigned int)slot->id != (unsigned int)pid ||
            slot->parent_id != me->id) {
            irq_restore(state);
            return -1;
        }
        if (slot->state == TASK_ZOMBIE) {
            int code = reap(slot);
            me->wait_pid = -1;
            irq_restore(state);
            return code;
        }
        me->wait_pid = pid;
        me->state = TASK_BLOCKED_WAIT;
        irq_restore(state);
        while (me->state == TASK_BLOCKED_WAIT) arch_cpu_wait();
    }
}

int proc_getpid(void) {
    return task_pool[scheduler_current()].id;
}

static int can_manage_task(const struct task *me, const struct task *target) {
    if (me->capabilities & CAP_TASK_ADMIN) return 1;

    int parent = target->parent_id;
    for (int depth = 0; parent > 0 && depth < MAX_TASKS; depth++) {
        if (parent == me->id) return 1;
        unsigned int slot = PID_SLOT((unsigned int)parent);
        if (slot == 0 || slot >= (unsigned int)task_pool_count) break;
        struct task *ancestor = &task_pool[slot];
        if (ancestor->state == TASK_FREE || ancestor->id != parent) break;
        parent = ancestor->parent_id;
    }
    return 0;
}

static struct task *managed_target(struct task *manager, int pid, int *error) {
    u32 slot = PID_SLOT((u32)pid);
    if (slot == 0 || slot >= (u32)task_pool_count) {
        *error = ESRCH;
        return 0;
    }
    struct task *target = &task_pool[slot];
    if (target->state == TASK_FREE || target->state == TASK_ZOMBIE ||
        target->id != pid) {
        *error = ESRCH;
        return 0;
    }
    if (!can_manage_task(manager, target)) {
        *error = EPERM;
        return 0;
    }
    *error = 0;
    return target;
}

int proc_cap_grant(int pid, unsigned int capabilities) {
    if (capabilities & ~CAP_BOOT_ALLOWED) return EINVAL;

    irq_state_t state = irq_save();
    struct task *me = &task_pool[scheduler_current()];
    int error;
    struct task *target = managed_target(me, pid, &error);
    if (!target) {
        irq_restore(state);
        return error;
    }
    if (capabilities & ~me->capabilities) {
        irq_restore(state);
        return EPERM;
    }

    target->capabilities |= capabilities;
    irq_restore(state);
    return 0;
}

int proc_irq_grant(int pid, unsigned int irq) {
    if (irq == 0 || irq >= 16) return EINVAL;

    irq_state_t state = irq_save();
    struct task *me = &task_pool[scheduler_current()];
    int error;
    struct task *target = managed_target(me, pid, &error);
    if (!target) {
        irq_restore(state);
        return error;
    }
    if (!(me->capabilities & CAP_RESOURCE_ADMIN) &&
        !(me->irq_rights & (1u << irq))) {
        irq_restore(state);
        return EPERM;
    }
    target->irq_rights |= 1u << irq;
    irq_restore(state);
    return 0;
}

int proc_ioport_grant(int pid, unsigned int port, unsigned int count) {
    irq_state_t state = irq_save();
    struct task *me = &task_pool[scheduler_current()];
    if (!(me->capabilities & CAP_RESOURCE_ADMIN)) {
        irq_restore(state);
        return EPERM;
    }
    int error;
    struct task *target = managed_target(me, pid, &error);
    if (!target) {
        irq_restore(state);
        return error;
    }
    int rc = arch_task_allow_io(target, port, count);
    irq_restore(state);
    return rc == 0 ? 0 : EINVAL;
}

int proc_mmio_grant(int pid, unsigned int phys, unsigned int length) {
    if (!length || (phys & 0xFFFu) || (length & 0xFFFu) ||
        length > 0x10000000u || phys + length < phys)
        return EINVAL;
    irq_state_t state = irq_save();
    struct task *me = &task_pool[scheduler_current()];
    if (!(me->capabilities & CAP_RESOURCE_ADMIN)) {
        irq_restore(state);
        return EPERM;
    }
    int error;
    struct task *target = managed_target(me, pid, &error);
    if (!target) {
        irq_restore(state);
        return error;
    }
    if (pmm_range_is_ram(phys, length)) {
        irq_restore(state);
        return EPERM;
    }

    for (int i = 0; i < MAX_MMIO_GRANTS; i++) {
        if (target->mmio[i].length == 0) {
            target->mmio[i].phys = phys;
            target->mmio[i].length = length;
            target->mmio[i].mapped_va = 0;
            irq_restore(state);
            return i + 1;
        }
    }
    irq_restore(state);
    return ENOSPC;
}

int proc_mmio_map(unsigned int handle, unsigned int user_va) {
    if (handle == 0 || handle > MAX_MMIO_GRANTS || (user_va & 0xFFFu))
        return EINVAL;

    irq_state_t state = irq_save();
    struct task *me = &task_pool[scheduler_current()];
    struct mmio_grant *grant = &me->mmio[handle - 1];
    if (!grant->length || grant->mapped_va ||
        !ua_valid(user_va, grant->length)) {
        irq_restore(state);
        return EINVAL;
    }

    for (unsigned int off = 0; off < grant->length; off += 4096) {
        if (paging_user_phys(me->page_dir, user_va + off)) {
            irq_restore(state);
            return EINVAL;
        }
    }

    unsigned int mapped = 0;
    while (mapped < grant->length) {
        if (paging_map_user(me->page_dir, grant->phys + mapped,
                            user_va + mapped, USER_MMIO_FLAGS) != 0) {
            for (unsigned int undo = 0; undo < mapped; undo += 4096)
                paging_unmap_user(me->page_dir, user_va + undo);
            irq_restore(state);
            return ENOMEM;
        }
        mapped += 4096;
    }
    grant->mapped_va = user_va;
    irq_restore(state);
    return 0;
}

int proc_dma_grant(int pid, unsigned int pages, unsigned int max_addr) {
    if (!pages || pages > 256 || max_addr < 0xFFFFFu) return EINVAL;

    irq_state_t state = irq_save();
    struct task *me = &task_pool[scheduler_current()];
    if (!(me->capabilities & CAP_RESOURCE_ADMIN)) {
        irq_restore(state);
        return EPERM;
    }
    int error;
    struct task *target = managed_target(me, pid, &error);
    if (!target) {
        irq_restore(state);
        return error;
    }
    if (target->dma_pages_used) {
        irq_restore(state);
        return EPERM;
    }
    target->dma_page_limit = pages;
    target->dma_pages_used = 0;
    target->dma_max_addr = max_addr;
    irq_restore(state);
    return 0;
}

int proc_dma_alloc(unsigned int pages, unsigned int user_va) {
    if (!pages || pages > 256 || (user_va & 0xFFFu)) return EINVAL;
    if (pages > 0xFFFFFFFFu / 4096u) return EINVAL;
    unsigned int length = pages * 4096u;
    if (!ua_valid(user_va, length)) return EINVAL;

    irq_state_t state = irq_save();
    struct task *me = &task_pool[scheduler_current()];
    if (!me->dma_page_limit || me->dma_pages_used > me->dma_page_limit ||
        pages > me->dma_page_limit - me->dma_pages_used) {
        irq_restore(state);
        return EPERM;
    }
    for (unsigned int off = 0; off < length; off += 4096) {
        if (paging_user_phys(me->page_dir, user_va + off)) {
            irq_restore(state);
            return EINVAL;
        }
    }

    unsigned int phys = pmm_alloc_contiguous(pages, me->dma_max_addr);
    if (!phys || phys > 0x7FFFFFFFu) {
        if (phys)
            for (unsigned int undo = 0; undo < length; undo += 4096)
                pmm_free_page(phys + undo);
        irq_restore(state);
        return ENOMEM;
    }

    unsigned int mapped = 0;
    while (mapped < length) {
        unsigned int *page = paging_kmap_b(phys + mapped);
        for (unsigned int i = 0; i < 4096 / 4; i++) page[i] = 0;
        if (paging_map_user(me->page_dir, phys + mapped, user_va + mapped,
                            USER_FLAGS) != 0) {
            for (unsigned int undo = 0; undo < mapped; undo += 4096)
                paging_unmap_user(me->page_dir, user_va + undo);
            for (unsigned int undo = 0; undo < length; undo += 4096)
                pmm_free_page(phys + undo);
            irq_restore(state);
            return ENOMEM;
        }
        mapped += 4096;
    }
    me->dma_pages_used += pages;
    irq_restore(state);
    return (int)phys;
}

int proc_kill(int pid) {
    irq_state_t state = irq_save();
    struct task *me = &task_pool[scheduler_current()];
    unsigned int slot_idx = PID_SLOT((unsigned int)pid);
    if (slot_idx == 0 || slot_idx >= (unsigned int)task_pool_count) {
        irq_restore(state);
        return ESRCH;
    }

    struct task *t = &task_pool[slot_idx];
    if (t->state == TASK_FREE || (unsigned int)t->id != (unsigned int)pid) {
        irq_restore(state);
        return ESRCH;
    }
    if ((unsigned int)me->id == (unsigned int)pid) {
        irq_restore(state);
        return proc_exit(137);
    }
    if (!can_manage_task(me, t)) {
        irq_restore(state);
        return EPERM;
    }
    if (t->state == TASK_ZOMBIE) {
        irq_restore(state);
        return 0;
    }

    terminate_task(t, 137, 0);
    irq_restore(state);
    return 0;
}
