#include "scheduler.h"
#include "task.h"

static int current_task = -1;

int scheduler_current(void) {
    return current_task;
}

void scheduler_set_current(int current) {
    current_task = current;
}

int scheduler_pick_next(int current) {
    // The scan itself lives in task.c, behind the pool lock: the queue is
    // shared by every CPU, so the asking CPU decides what it may take and
    // the answer is read under the same lock that guards the claims.
    return task_pick_next(current, scheduler_cpu_id());
}
