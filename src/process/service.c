#include "service.h"
#include "capability.h"
#include "irq.h"
#include "scheduler.h"
#include "task.h"
#include "protos.h"

static int service_owners[SERVICE_MAX];

void service_init(void) {
    for (int i = 0; i < SERVICE_MAX; i++) service_owners[i] = -1;
}

int service_register(unsigned int service) {
    if (service == 0 || service >= SERVICE_MAX) return EINVAL;

    int current_task = scheduler_current();
    if (current_task < 0 || current_task >= task_pool_count) return ESRCH;
    struct task *task = &task_pool[current_task];
    if (!(task->capabilities & CAP_SERVICE_REGISTER)) return EPERM;
    int owner = task->id;
    irq_state_t state = irq_save();
    int current = service_owners[service];
    if (current >= 0 && current != owner) {
        irq_restore(state);
        return EPERM;
    }
    service_owners[service] = owner;
    irq_restore(state);
    return 0;
}

int service_lookup(unsigned int service) {
    if (service == 0 || service >= SERVICE_MAX) return EINVAL;

    irq_state_t state = irq_save();
    int owner = service_owners[service];
    if (owner >= 0) {
        unsigned int slot = PID_SLOT((unsigned int)owner);
        if (slot >= (unsigned int)task_pool_count ||
            task_pool[slot].state == TASK_FREE ||
            task_pool[slot].state == TASK_ZOMBIE ||
            task_pool[slot].id != owner) {
            service_owners[service] = -1;
            owner = -1;
        }
    }
    irq_restore(state);
    return owner >= 0 ? owner : ESRCH;
}

void service_release_owner(unsigned int full_pid) {
    irq_state_t state = irq_save();
    for (int i = 1; i < SERVICE_MAX; i++) {
        if (service_owners[i] == (int)full_pid) service_owners[i] = -1;
    }
    irq_restore(state);
}
