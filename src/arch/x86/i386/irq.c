#include "irq.h"
#include "ipc.h"
#include "task.h"
#include "scheduler.h"
#include "protos.h"

static int irq_owner[16];
static unsigned int irq_pending[16];

static void outb(unsigned short port, unsigned char val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

void irq_init(void) {
    for (int i = 0; i < 16; i++) {
        irq_owner[i] = -1;
        irq_pending[i] = 0;
    }
}

int irq_register(unsigned int irq) {
    if (irq == 0 || irq >= 16) return -1;
    irq_state_t state = irq_save();
    struct task *me = &task_pool[scheduler_current()];
    if (!(me->irq_rights & (1u << irq))) {
        irq_restore(state);
        return EPERM;
    }
    if (irq_owner[irq] >= 0) {
        irq_restore(state);
        return -1;
    }
    irq_owner[irq] = me->id;
    irq_pending[irq] = 0;
    irq_restore(state);
    return 0;
}

void irq_release_owner(unsigned int full_pid) {
    irq_state_t state = irq_save();
    for (int i = 0; i < 16; i++) {
        if (irq_owner[i] == (int)full_pid) {
            irq_owner[i] = -1;
            irq_pending[i] = 0;
        }
    }
    irq_restore(state);
}

void irq_handler_main(unsigned int irq) {
    if (irq < 16 && irq_owner[irq] >= 0) {
        struct message m;
        m.from_id = 0;
        m.type = MSG_IRQ;
        for (int i = 0; i < 56; i++) m.data[i] = 0;
        m.data[0] = (unsigned char)irq;
        if (ipc_try_deliver((unsigned int)irq_owner[irq], &m) < 0 &&
            irq_pending[irq] != 0xFFFFFFFFu)
            irq_pending[irq]++;
    }
    if (irq >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

unsigned int irq_poll_pending(void) {
    int me = scheduler_current();
    for (int i = 1; i < 16; i++) {
        if (irq_owner[i] == task_pool[me].id && irq_pending[i] > 0) {
            irq_pending[i]--;
            return (unsigned int)i;
        }
    }
    return 0;
}
