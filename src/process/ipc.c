#include "ipc.h"
#include "arch_cpu.h"
#include "task.h"
#include "scheduler.h"
#include "irq.h"
#include "protos.h"
#include "uaccess.h"
#include "paging.h"
#include "service.h"

static int msg_present(paddr_t pd_phys, vaddr_t va) {
    irq_state_t state = irq_save();
    int valid = paging_user_phys(pd_phys, va) != 0;
    if (valid && (va & 0xFFF) + sizeof(struct message) > 4096)
        valid = paging_user_phys(pd_phys, (va & ~(vaddr_t)0xFFF) + 4096) != 0;
    irq_restore(state);
    return valid;
}

static int msg_writable(paddr_t pd_phys, vaddr_t va) {
    irq_state_t state = irq_save();
    int valid = paging_user_writable(pd_phys, va) || paging_handle_cow(pd_phys, va);
    if (valid && (va & 0xFFF) + sizeof(struct message) > 4096) {
        vaddr_t next = (va & ~(vaddr_t)0xFFF) + 4096;
        valid = paging_user_writable(pd_phys, next) || paging_handle_cow(pd_phys, next);
    }
    irq_restore(state);
    return valid;
}

static paddr_t me_pd(void) {
    return task_pool[scheduler_current()].page_dir;
}

static int send_would_deadlock(struct task *sender, struct task *destination) {
    struct task *cursor = destination;
    for (int depth = 0; depth < MAX_TASKS; depth++) {
        if (cursor == sender) return 1;
        if (cursor->state != TASK_BLOCKED_SEND || !cursor->send_to) return 0;
        u32 slot = PID_SLOT(cursor->send_to);
        if (slot == 0 || slot >= (u32)task_pool_count) return 0;
        struct task *next = &task_pool[slot];
        if (next->state == TASK_FREE || (u32)next->id != cursor->send_to) return 0;
        cursor = next;
    }
    return 1;
}

static int deliver_sender(struct task *receiver, struct task *sender,
                          struct message *destination) {
    struct message message;
    int result = ua_copy_from(sender->page_dir, &message,
                              (vaddr_t)(uptr_t)sender->pending_msg,
                              sizeof(message));
    if (!result) {
        message.from_id = task_full_pid(sender);
        result = ua_copy_to(receiver->page_dir, (vaddr_t)(uptr_t)destination,
                            &message, sizeof(message));
    }
    sender->send_result = result ? EINVAL : 0;
    sender->send_to = 0;
    sender->send_deadline = 0;
    sender->pending_msg = 0;
    sender->state = TASK_RUNNING;
    return result ? EINVAL : 0;
}

static void died_fill(struct message *m, unsigned int id) {
    m->from_id = id;
    m->type = MSG_DIED;
    for (int i = 0; i < 56; i++) m->data[i] = 0;
}

void ipc_flush_task(unsigned int id) {
    unsigned int slot = PID_SLOT(id);
    if (slot >= (unsigned int)task_pool_count) return;
    struct task *dead = &task_pool[slot];
    irq_state_t state = irq_save();
    for (int i = 0; i < task_pool_count; i++) {
        struct task *d = &task_pool[i];
        struct task *prev = 0;
        struct task *w = d->wq_head;
        while (w) {
            if (w == dead) {
                if (prev) prev->next = w->next;
                else d->wq_head = w->next;
                if (d->wq_tail == w) d->wq_tail = prev;
                w->next = 0;
                if (d != dead && d->state == TASK_BLOCKED_RECV &&
                    d->recv_buf && d->recv_expect == 0) {
                    struct message m;
                    died_fill(&m, id);
                    ua_copy_to(d->page_dir, (vaddr_t)(uptr_t)d->recv_buf, &m, sizeof(struct message));
                    d->recv_buf = 0;
                    d->state = TASK_RUNNING;
                }
                break;
            }
            prev = w;
            w = w->next;
        }
    }
    struct task *w = dead->wq_head;
    while (w) {
        struct task *nx = w->next;
        if (w->state == TASK_BLOCKED_SEND) {
            w->send_result = ESRCH;
            w->send_to = 0;
            w->send_deadline = 0;
            w->pending_msg = 0;
            w->state = TASK_RUNNING;
        }
        w->next = 0;
        w = nx;
    }
    dead->wq_head = 0;
    dead->wq_tail = 0;
    for (int i = 0; i < task_pool_count; i++) {
        struct task *d = &task_pool[i];
        if (d->state == TASK_BLOCKED_RECV && d->recv_expect == id && d->recv_buf) {
            struct message m;
            died_fill(&m, id);
            ua_copy_to(d->page_dir, (vaddr_t)(uptr_t)d->recv_buf, &m, sizeof(struct message));
            d->recv_buf = 0;
            d->recv_expect = 0;
            d->state = TASK_RUNNING;
        }
    }
    irq_restore(state);
}

void ipc_tick(unsigned int now) {
    irq_state_t state = irq_save();
    for (int i = 1; i < task_pool_count; i++) {
        struct task *sender = &task_pool[i];
        if (sender->state != TASK_BLOCKED_SEND || !sender->send_deadline ||
            (i32)(now - sender->send_deadline) < 0)
            continue;
        u32 slot = PID_SLOT(sender->send_to);
        if (slot < (u32)task_pool_count) {
            struct task *destination = &task_pool[slot];
            struct task *previous = 0;
            struct task *cursor = destination->wq_head;
            while (cursor && cursor != sender) {
                previous = cursor;
                cursor = cursor->next;
            }
            if (cursor) {
                if (previous) previous->next = cursor->next;
                else destination->wq_head = cursor->next;
                if (destination->wq_tail == cursor) destination->wq_tail = previous;
            }
        }
        sender->next = 0;
        sender->pending_msg = 0;
        sender->send_to = 0;
        sender->send_deadline = 0;
        sender->send_result = ETIMEDOUT;
        sender->state = TASK_RUNNING;
    }
    irq_restore(state);
}

static int ipc_send_internal(unsigned int dest, const struct message *msg,
                             unsigned int timeout_ticks) {
    if (!msg) return -1;
    if (!ua_valid((vaddr_t)(uptr_t)msg, sizeof(struct message)) ||
        !msg_present(me_pd(), (vaddr_t)(uptr_t)msg)) return -1;

    unsigned int slot = PID_SLOT(dest);
    if (slot == 0 || slot >= (unsigned int)task_pool_count) return -1;
    unsigned int me = scheduler_current();
    if (slot == me) return -2;

    struct task *s = &task_pool[me];
    struct task *d = &task_pool[slot];
    struct message xm;
    int fs_pid = service_lookup(SERVICE_FS);

    irq_state_t state = irq_save();
    if (d->state == TASK_FREE || d->state == TASK_ZOMBIE) { irq_restore(state); return ESRCH; }
    if ((unsigned int)d->id != dest) { irq_restore(state); return ESRCH; }
    int gated = d->exec_gate && s->id != fs_pid;
    if (d->recv_expect && d->recv_expect != (unsigned int)s->id) gated = 1;

    if (!gated && d->state == TASK_BLOCKED_RECV && d->recv_buf) {
        if (ua_copy_from(s->page_dir, &xm, (vaddr_t)(uptr_t)msg,
                         sizeof(struct message)) != 0) {
            irq_restore(state);
            return EINVAL;
        }
        xm.from_id = (unsigned int)s->id;
        if (ua_copy_to(d->page_dir, (vaddr_t)(uptr_t)d->recv_buf, &xm,
                       sizeof(struct message)) != 0) {
            irq_restore(state);
            return EINVAL;
        }
        d->recv_buf = 0;
        d->recv_expect = 0;
        d->state = TASK_RUNNING;
        irq_restore(state);
        return 0;
    }

    if (send_would_deadlock(s, d)) {
        irq_restore(state);
        return EDEADLK;
    }
    s->pending_msg = msg;
    s->send_result = 0;
    s->send_to = dest;
    s->send_deadline = timeout_ticks ? scheduler_now() + timeout_ticks : 0;
    s->state = TASK_BLOCKED_SEND;
    s->next = 0;
    if (d->wq_tail) d->wq_tail->next = s;
    else d->wq_head = s;
    d->wq_tail = s;
    irq_restore(state);

    while (s->state == TASK_BLOCKED_SEND)
        arch_cpu_wait();
    return s->send_result;
}

int ipc_send(unsigned int dest, const struct message *msg) {
    return ipc_send_internal(dest, msg, 0);
}

int ipc_send_timeout(unsigned int dest, const struct message *msg,
                     unsigned int timeout_ticks) {
    if (!timeout_ticks) return EINVAL;
    return ipc_send_internal(dest, msg, timeout_ticks);
}

int ipc_send_nb(unsigned int dest, const struct message *msg) {
    if (!msg) return -1;
    if (!ua_valid((vaddr_t)(uptr_t)msg, sizeof(struct message)) ||
        !msg_present(me_pd(), (vaddr_t)(uptr_t)msg)) return -1;

    unsigned int slot = PID_SLOT(dest);
    if (slot == 0 || slot >= (unsigned int)task_pool_count) return -1;
    unsigned int me = scheduler_current();
    if (slot == me) return -2;

    struct task *s = &task_pool[me];
    struct task *d = &task_pool[slot];
    struct message xm;
    int rc = -1;
    int fs_pid = service_lookup(SERVICE_FS);

    irq_state_t state = irq_save();
    if (d->state == TASK_FREE || d->state == TASK_ZOMBIE) { irq_restore(state); return ESRCH; }
    if ((unsigned int)d->id != dest) { irq_restore(state); return ESRCH; }
    int gated = d->exec_gate && s->id != fs_pid;
    if (d->recv_expect && d->recv_expect != (unsigned int)s->id) gated = 1;

    if (!gated && d->state == TASK_BLOCKED_RECV && d->recv_buf) {
        if (ua_copy_from(s->page_dir, &xm, (vaddr_t)(uptr_t)msg,
                         sizeof(struct message)) != 0) {
            irq_restore(state);
            return EINVAL;
        }
        xm.from_id = (unsigned int)s->id;
        if (ua_copy_to(d->page_dir, (vaddr_t)(uptr_t)d->recv_buf, &xm,
                       sizeof(struct message)) != 0) {
            irq_restore(state);
            return EINVAL;
        }
        d->recv_buf = 0;
        d->recv_expect = 0;
        d->state = TASK_RUNNING;
        rc = 0;
    }
    irq_restore(state);
    return rc;
}

int ipc_try_deliver(unsigned int dest, const struct message *msg) {
    if (!msg) return -1;
    unsigned int slot = PID_SLOT(dest);
    if (slot >= (unsigned int)task_pool_count) return -1;

    struct task *d = &task_pool[slot];
    if ((unsigned int)d->id != dest) return -1;
    if (d->state == TASK_BLOCKED_RECV && d->recv_buf) {
        if (ua_copy_to(d->page_dir, (vaddr_t)(uptr_t)d->recv_buf,
                       msg, sizeof(struct message)) != 0)
            return -1;
        d->recv_buf = 0;
        d->recv_expect = 0;
        d->state = TASK_RUNNING;
        return 0;
    }
    return -1;
}

int ipc_recv_from(unsigned int expect, struct message *msg) {
    if (!msg) return -1;
    if (!ua_valid((vaddr_t)(uptr_t)msg, sizeof(struct message)) ||
        !msg_present(me_pd(), (vaddr_t)(uptr_t)msg) ||
        !msg_writable(me_pd(), (vaddr_t)(uptr_t)msg)) return -1;

    unsigned int me = scheduler_current();
    struct task *s = &task_pool[me];

    s->recv_expect = expect;
    irq_state_t state = irq_save();

    unsigned int pirq = irq_poll_pending();
    if (pirq) {
        struct message notification;
        notification.from_id = 0;
        notification.type = MSG_IRQ;
        for (int i = 0; i < 56; i++) notification.data[i] = 0;
        notification.data[0] = (unsigned char)pirq;
        s->recv_expect = 0;
        int copied = ua_copy_to(
            s->page_dir, (vaddr_t)(uptr_t)msg,
            &notification, sizeof(notification));
        irq_restore(state);
        return copied ? EINVAL : 0;
    }

    if (expect) {
        unsigned int es = PID_SLOT(expect);
        if (es == 0 || es >= (unsigned int)task_pool_count ||
            task_pool[es].state == TASK_FREE || task_pool[es].state == TASK_ZOMBIE ||
            task_full_pid(&task_pool[es]) != expect) {
            struct message notification;
            died_fill(&notification, expect);
            int copied = ua_copy_to(
                s->page_dir, (vaddr_t)(uptr_t)msg,
                &notification, sizeof(notification));
            s->recv_expect = 0;
            irq_restore(state);
            return copied ? EINVAL : 0;
        }
    }

    struct task *prev = 0;
    struct task *w = s->wq_head;
    while (w && expect && task_full_pid(w) != expect) {
        prev = w;
        w = w->next;
    }
    if (w) {
        if (prev) prev->next = w->next;
        else s->wq_head = w->next;
        if (s->wq_tail == w) {
            if (!prev) {
                s->wq_tail = 0;
            } else {
                s->wq_tail = prev;
            }
        }
        w->next = 0;
        int result = deliver_sender(s, w, msg);
        s->recv_expect = 0;
        irq_restore(state);
        return result;
    }

    s->recv_buf = msg;
    s->state = TASK_BLOCKED_RECV;
    irq_restore(state);

    while (s->state == TASK_BLOCKED_RECV)
        arch_cpu_wait();
    s->recv_expect = 0;
    return 0;
}

int ipc_recv(struct message *msg) {
    if (!msg) return -1;
    if (!ua_valid((vaddr_t)(uptr_t)msg, sizeof(struct message)) ||
        !msg_present(me_pd(), (vaddr_t)(uptr_t)msg) ||
        !msg_writable(me_pd(), (vaddr_t)(uptr_t)msg)) return -1;

    unsigned int me = scheduler_current();
    struct task *s = &task_pool[me];

    irq_state_t state = irq_save();

    unsigned int pirq = irq_poll_pending();
    if (pirq) {
        struct message notification;
        notification.from_id = 0;
        notification.type = MSG_IRQ;
        for (int i = 0; i < 56; i++) notification.data[i] = 0;
        notification.data[0] = (unsigned char)pirq;
        int copied = ua_copy_to(
            s->page_dir, (vaddr_t)(uptr_t)msg,
            &notification, sizeof(notification));
        irq_restore(state);
        return copied ? EINVAL : 0;
    }

    struct task *w = s->wq_head;
    if (w) {
        s->wq_head = w->next;
        if (!s->wq_head) s->wq_tail = 0;
        w->next = 0;
        int result = deliver_sender(s, w, msg);
        irq_restore(state);
        return result;
    }

    s->recv_buf = msg;
    s->state = TASK_BLOCKED_RECV;
    irq_restore(state);

    while (s->state == TASK_BLOCKED_RECV)
        arch_cpu_wait();
    return 0;
}
