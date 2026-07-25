#include "ipc.h"
#include "task.h"
#include "scheduler.h"
#include "irq.h"
#include "protos.h"

static unsigned char xlat_tmp[64];

static unsigned int read_cr3(void) {
    unsigned int v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static void write_cr3(unsigned int v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v));
}

static void msg_copy(unsigned char *dst, const unsigned char *src) {
    for (unsigned int i = 0; i < sizeof(struct message); i++)
        dst[i] = src[i];
}

static void tmp_set_from(unsigned int id) {
    xlat_tmp[0] = (unsigned char)(id & 0xFF);
    xlat_tmp[1] = (unsigned char)((id >> 8) & 0xFF);
    xlat_tmp[2] = (unsigned char)((id >> 16) & 0xFF);
    xlat_tmp[3] = (unsigned char)((id >> 24) & 0xFF);
}

static void intr_off(void) { __asm__ volatile("cli"); }
static void intr_on(void) { __asm__ volatile("sti"); }

static void copy_in(const void *src, unsigned int src_pd) {
    unsigned int saved = read_cr3();
    write_cr3(src_pd);
    msg_copy(xlat_tmp, (const unsigned char *)src);
    write_cr3(saved);
}

static void copy_out(unsigned char *dst, unsigned int dst_pd) {
    unsigned int saved = read_cr3();
    write_cr3(dst_pd);
    msg_copy(dst, xlat_tmp);
    write_cr3(saved);
}

void ipc_flush_task(unsigned int id) {
    intr_off();
    for (int i = 0; i < task_pool_count; i++) {
        struct task *d = &task_pool[i];
        struct task *prev = 0;
        struct task *w = d->wq_head;
        while (w) {
            if ((unsigned int)(w - task_pool) == id) {
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
    struct task *t = &task_pool[id];
    struct task *w = t->wq_head;
    while (w) {
        struct task *nx = w->next;
        if (w->state == TASK_BLOCKED_SEND) w->state = TASK_RUNNING;
        w->next = 0;
        w = nx;
    }
    t->wq_head = 0;
    t->wq_tail = 0;
    intr_on();
}

int ipc_send(unsigned int dest, const struct message *msg) {
    if (!msg || dest >= (unsigned int)task_pool_count) return -1;

    unsigned int me = scheduler_current();
    if (dest == me) return -2;

    struct task *d = &task_pool[dest];
    if (d->state == TASK_FREE || d->state == TASK_ZOMBIE) return -3;
    struct task *s = &task_pool[me];
    int gated = d->exec_gate && me != FS_SERVER_ID;
    if (d->recv_expect && d->recv_expect != me) gated = 1;

    intr_off();
    if (!gated && d->state == TASK_BLOCKED_RECV && d->recv_buf) {
        msg_copy(xlat_tmp, (const unsigned char *)msg);
        tmp_set_from(me);
        copy_out((unsigned char *)d->recv_buf, d->page_dir);
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
    if (!msg || dest >= (unsigned int)task_pool_count) return -1;

    unsigned int me = scheduler_current();
    if (dest == me) return -2;

    struct task *d = &task_pool[dest];
    if (d->state == TASK_FREE || d->state == TASK_ZOMBIE) return -3;
    int gated = d->exec_gate && me != FS_SERVER_ID;
    if (d->recv_expect && d->recv_expect != me) gated = 1;
    int rc = -1;

    intr_off();
    if (!gated && d->state == TASK_BLOCKED_RECV && d->recv_buf) {
        msg_copy(xlat_tmp, (const unsigned char *)msg);
        tmp_set_from(me);
        copy_out((unsigned char *)d->recv_buf, d->page_dir);
        d->state = TASK_RUNNING;
        rc = 0;
    }
    intr_on();
    return rc;
}

int ipc_try_deliver(unsigned int dest, const struct message *msg) {
    if (!msg || dest >= (unsigned int)task_pool_count) return -1;

    struct task *d = &task_pool[dest];
    if (d->state == TASK_BLOCKED_RECV && d->recv_buf) {
        msg_copy(xlat_tmp, (const unsigned char *)msg);
        copy_out((unsigned char *)d->recv_buf, d->page_dir);
        d->state = TASK_RUNNING;
        return 0;
    }
    return -1;
}

int ipc_recv_from(unsigned int expect, struct message *msg) {
    if (!msg) return -1;

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
    while (w && (unsigned int)(w - task_pool) != expect) {
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
        copy_in(w->pending_msg, w->page_dir);
        msg_copy((unsigned char *)msg, xlat_tmp);
        msg->from_id = (unsigned int)(w - task_pool);
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
        copy_in(w->pending_msg, w->page_dir);
        msg_copy((unsigned char *)msg, xlat_tmp);
        msg->from_id = (unsigned int)(w - task_pool);
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
