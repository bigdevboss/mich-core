#include "ipc.h"
#include "task.h"
#include "scheduler.h"
#include "irq.h"
#include "protos.h"
#include "uaccess.h"
#include "paging.h"

static struct message xlat_msg;

static int msg_present(unsigned int pd_phys, unsigned int va) {
    if (!paging_user_phys(pd_phys, va)) return 0;
    if ((va & 0xFFF) + sizeof(struct message) > 4096)
        if (!paging_user_phys(pd_phys, (va & 0xFFFFF000) + 4096)) return 0;
    return 1;
}

static unsigned int me_pd(void) {
    return task_pool[scheduler_current()].page_dir;
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
    intr_off();
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
                break;
            }
            prev = w;
            w = w->next;
        }
    }
    struct task *w = dead->wq_head;
    while (w) {
        struct task *nx = w->next;
        if (w->state == TASK_BLOCKED_SEND) w->state = TASK_RUNNING;
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
            ua_copy_to(d->page_dir, (unsigned int)d->recv_buf, &m, sizeof(struct message));
            d->state = TASK_RUNNING;
        }
    }
    intr_on();
}

int ipc_send(unsigned int dest, const struct message *msg) {
    if (!msg) return -1;
    if (!ua_valid((unsigned int)msg, sizeof(struct message)) ||
        !msg_present(me_pd(), (unsigned int)msg)) return -1;

    unsigned int slot = PID_SLOT(dest);
    if (slot == 0 || slot >= (unsigned int)task_pool_count) return -1;
    unsigned int me = scheduler_current();
    if (slot == me) return -2;

    struct task *s = &task_pool[me];
    struct task *d = &task_pool[slot];
    if (d->state == TASK_FREE || d->state == TASK_ZOMBIE) return -3;
    if ((unsigned int)d->id != dest) return -3;
    int gated = d->exec_gate && (int)me != FS_SERVER_ID;
    if (d->recv_expect && d->recv_expect != (unsigned int)s->id) gated = 1;

    intr_off();
    if (!gated && d->state == TASK_BLOCKED_RECV && d->recv_buf) {
        ua_copy_from(s->page_dir, &xlat_msg, (unsigned int)msg, sizeof(struct message));
        xlat_msg.from_id = (unsigned int)s->id;
        ua_copy_to(d->page_dir, (unsigned int)d->recv_buf, &xlat_msg, sizeof(struct message));
        d->state = TASK_RUNNING;
        intr_on();
        return 0;
    }

    s->pending_msg = msg;
    s->state = TASK_BLOCKED_SEND;
    s->next = 0;
    if (d->wq_tail) d->wq_tail->next = s;
    else d->wq_head = s;
    d->wq_tail = s;
    intr_on();

    while (s->state == TASK_BLOCKED_SEND)
        __asm__ volatile("sti; hlt");
    return 0;
}

int ipc_send_nb(unsigned int dest, const struct message *msg) {
    if (!msg) return -1;
    if (!ua_valid((unsigned int)msg, sizeof(struct message)) ||
        !msg_present(me_pd(), (unsigned int)msg)) return -1;

    unsigned int slot = PID_SLOT(dest);
    if (slot == 0 || slot >= (unsigned int)task_pool_count) return -1;
    unsigned int me = scheduler_current();
    if (slot == me) return -2;

    struct task *s = &task_pool[me];
    struct task *d = &task_pool[slot];
    if (d->state == TASK_FREE || d->state == TASK_ZOMBIE) return -3;
    if ((unsigned int)d->id != dest) return -3;
    int gated = d->exec_gate && (int)me != FS_SERVER_ID;
    if (d->recv_expect && d->recv_expect != (unsigned int)s->id) gated = 1;
    int rc = -1;

    intr_off();
    if (!gated && d->state == TASK_BLOCKED_RECV && d->recv_buf) {
        ua_copy_from(s->page_dir, &xlat_msg, (unsigned int)msg, sizeof(struct message));
        xlat_msg.from_id = (unsigned int)s->id;
        ua_copy_to(d->page_dir, (unsigned int)d->recv_buf, &xlat_msg, sizeof(struct message));
        d->state = TASK_RUNNING;
        rc = 0;
    }
    intr_on();
    return rc;
}

int ipc_try_deliver(unsigned int dest, const struct message *msg) {
    if (!msg) return -1;
    unsigned int slot = PID_SLOT(dest);
    if (slot >= (unsigned int)task_pool_count) return -1;

    struct task *d = &task_pool[slot];
    if ((unsigned int)d->id != dest) return -1;
    if (d->state == TASK_BLOCKED_RECV && d->recv_buf) {
        ua_copy_to(d->page_dir, (unsigned int)d->recv_buf, msg, sizeof(struct message));
        d->state = TASK_RUNNING;
        return 0;
    }
    return -1;
}

int ipc_recv_from(unsigned int expect, struct message *msg) {
    if (!msg) return -1;
    if (!ua_valid((unsigned int)msg, sizeof(struct message)) ||
        !msg_present(me_pd(), (unsigned int)msg)) return -1;

    unsigned int me = scheduler_current();
    struct task *s = &task_pool[me];

    s->recv_expect = expect;
    intr_off();

    unsigned int pirq = irq_poll_pending();
    if (pirq) {
        s->recv_expect = 0;
        msg->from_id = 0;
        msg->type = MSG_IRQ;
        for (int i = 0; i < 56; i++) msg->data[i] = 0;
        msg->data[0] = (unsigned char)pirq;
        intr_on();
        return 0;
    }

    struct task *prev = 0;
    struct task *w = s->wq_head;
    while (w && task_full_pid(w) != expect) {
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
        ua_copy_from(w->page_dir, &xlat_msg, (unsigned int)w->pending_msg, sizeof(struct message));
        xlat_msg.from_id = task_full_pid(w);
        ua_copy_to(s->page_dir, (unsigned int)msg, &xlat_msg, sizeof(struct message));
        w->state = TASK_RUNNING;
        s->recv_expect = 0;
        intr_on();
        return 0;
    }

    s->recv_buf = msg;
    s->state = TASK_BLOCKED_RECV;
    intr_on();

    while (s->state == TASK_BLOCKED_RECV)
        __asm__ volatile("sti; hlt");
    s->recv_buf = 0;
    s->recv_expect = 0;
    return 0;
}

int ipc_recv(struct message *msg) {
    if (!msg) return -1;
    if (!ua_valid((unsigned int)msg, sizeof(struct message)) ||
        !msg_present(me_pd(), (unsigned int)msg)) return -1;

    unsigned int me = scheduler_current();
    struct task *s = &task_pool[me];

    intr_off();

    unsigned int pirq = irq_poll_pending();
    if (pirq) {
        msg->from_id = 0;
        msg->type = MSG_IRQ;
        for (int i = 0; i < 56; i++) msg->data[i] = 0;
        msg->data[0] = (unsigned char)pirq;
        intr_on();
        return 0;
    }

    struct task *w = s->wq_head;
    if (w) {
        s->wq_head = w->next;
        if (!s->wq_head) s->wq_tail = 0;
        ua_copy_from(w->page_dir, &xlat_msg, (unsigned int)w->pending_msg, sizeof(struct message));
        xlat_msg.from_id = task_full_pid(w);
        ua_copy_to(s->page_dir, (unsigned int)msg, &xlat_msg, sizeof(struct message));
        w->state = TASK_RUNNING;
        intr_on();
        return 0;
    }

    s->recv_buf = msg;
    s->state = TASK_BLOCKED_RECV;
    intr_on();

    while (s->state == TASK_BLOCKED_RECV)
        __asm__ volatile("sti; hlt");
    s->recv_buf = 0;
    return 0;
}
