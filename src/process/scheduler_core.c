#include "scheduler.h"
#include "task.h"

static int current_task = -1;

int scheduler_current(void) {
    return current_task;
}

void scheduler_set_current(int current) {
    current_task = current;
}

static int this_cpu;

void scheduler_set_this_cpu(int cpu) {
    this_cpu = cpu;
}

int scheduler_pick_next(int current) {
    if (task_pool_count <= 0) return -1;
    int idle = -1;
    for (int offset = 1; offset <= task_pool_count; offset++) {
        int candidate = (current + offset) % task_pool_count;
        if (task_pool[candidate].state != TASK_RUNNING) continue;
        int owner = task_pool[candidate].on_cpu;
        if (owner != TASK_CPU_NONE && owner != this_cpu) continue;
        // The idle task is always runnable but does no useful work. Remember
        // it and keep scanning so a freshly woken task is never passed over
        // for the ring-3 busy-loop, which would otherwise hold the CPU until
        // the next timer tick (~10ms of cross-task wake latency).
        if (task_pool[candidate].is_idle) {
            if (idle < 0) idle = candidate;
            continue;
        }
        return candidate;
    }
    return idle;
}
