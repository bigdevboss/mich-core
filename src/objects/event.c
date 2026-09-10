#include "event.h"
#include "task.h"
#include "irq.h"

struct event_state {
    u32 mode;
    u32 signaled;
    u32 waiters;
    u32 active;
};

static struct event_state events[EVENT_MAX];
static event_wake_fn wake_task;
static struct kernel_object *wait_sets[MAX_TASKS][EVENT_WAIT_MANY_MAX];
static u32 wait_counts[MAX_TASKS];
static u32 wait_deadlines[MAX_TASKS];
static u8 wait_timed[MAX_TASKS];

static struct event_state *state_for(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_EVENT ||
        !object->value || object->value > EVENT_MAX)
        return 0;
    struct event_state *state = &events[object->value - 1];
    return state->active ? state : 0;
}

static i64 wait_index(u32 slot, const struct kernel_object *object) {
    if (slot >= MAX_TASKS) return -1;
    for (u32 index = 0; index < wait_counts[slot]; index++)
        if (wait_sets[slot][index] == object) return index;
    return -1;
}

static void clear_wait_set(u32 slot) {
    if (slot >= MAX_TASKS) return;
    u32 bit = 1u << slot;
    for (u32 index = 0; index < wait_counts[slot]; index++) {
        struct event_state *state = state_for(wait_sets[slot][index]);
        if (state) state->waiters &= ~bit;
        wait_sets[slot][index] = 0;
    }
    wait_counts[slot] = 0;
    wait_deadlines[slot] = 0;
    wait_timed[slot] = 0;
}

static void wake_slot(u32 slot, i64 result) {
    if (slot >= (u32)task_pool_count ||
        task_pool[slot].state != TASK_BLOCKED_EVENT)
        return;
    clear_wait_set(slot);
    task_pool[slot].state = TASK_RUNNING;
    if (wake_task) wake_task(slot, result);
}

static void event_destroy(struct kernel_object *object) {
    irq_state_t irq = irq_save();
    if (!object || object->type != KOBJECT_EVENT ||
        !object->value || object->value > EVENT_MAX) {
        irq_restore(irq);
        return;
    }
    struct event_state *state = &events[object->value - 1];
    if (!state->active) {
        irq_restore(irq);
        return;
    }
    u32 waiters = state->waiters;
    state->waiters = 0;
    state->signaled = 0;
    state->active = 0;
    for (u32 slot = 0; slot < MAX_TASKS; slot++)
        if (waiters & (1u << slot)) wake_slot(slot, -3);
    irq_restore(irq);
}

void event_init(event_wake_fn wake) {
    wake_task = wake;
    for (u32 slot = 0; slot < MAX_TASKS; slot++) {
        wait_counts[slot] = 0;
        wait_deadlines[slot] = 0;
        wait_timed[slot] = 0;
        for (u32 index = 0; index < EVENT_WAIT_MANY_MAX; index++)
            wait_sets[slot][index] = 0;
    }
    for (u32 index = 0; index < EVENT_MAX; index++) {
        events[index].mode = EVENT_AUTO_RESET;
        events[index].signaled = 0;
        events[index].waiters = 0;
        events[index].active = 0;
    }
}

struct kernel_object *event_create(u32 mode, int signaled) {
    if (mode != EVENT_AUTO_RESET && mode != EVENT_MANUAL_RESET) return 0;
    for (u32 index = 0; index < EVENT_MAX; index++) {
        struct event_state *state = &events[index];
        if (state->active) continue;
        state->mode = mode;
        state->signaled = signaled != 0;
        state->waiters = 0;
        state->active = 1;
        struct kernel_object *object =
            object_create(KOBJECT_EVENT, index + 1, event_destroy);
        if (!object) state->active = 0;
        return object;
    }
    return 0;
}

int event_wait_many(struct kernel_object **objects, u32 count, u32 task_slot,
                    u32 deadline, int timed, u32 *ready_index) {
    irq_state_t irq = irq_save();
    if (!objects || !count || count > EVENT_WAIT_MANY_MAX ||
        task_slot >= (u32)task_pool_count || task_slot >= MAX_TASKS ||
        task_pool[task_slot].state != TASK_RUNNING || wait_counts[task_slot]) {
        irq_restore(irq);
        return -1;
    }
    for (u32 index = 0; index < count; index++) {
        struct event_state *state = state_for(objects[index]);
        if (!state) {
            irq_restore(irq);
            return -1;
        }
        for (u32 prior = 0; prior < index; prior++)
            if (objects[prior] == objects[index]) {
                irq_restore(irq);
                return -1;
            }
        if (state->signaled) {
            if (state->mode == EVENT_AUTO_RESET) state->signaled = 0;
            if (ready_index) *ready_index = index;
            irq_restore(irq);
            return 0;
        }
    }
    u32 bit = 1u << task_slot;
    for (u32 index = 0; index < count; index++) {
        struct event_state *state = state_for(objects[index]);
        state->waiters |= bit;
        wait_sets[task_slot][index] = objects[index];
    }
    wait_counts[task_slot] = count;
    wait_deadlines[task_slot] = deadline;
    wait_timed[task_slot] = timed != 0;
    task_pool[task_slot].state = TASK_BLOCKED_EVENT;
    irq_restore(irq);
    return 1;
}

int event_wait(struct kernel_object *object, u32 task_slot) {
    struct kernel_object *objects[1] = {object};
    u32 ready = 0;
    return event_wait_many(objects, 1, task_slot, 0, 0, &ready);
}

int event_wait_timeout(struct kernel_object *object, u32 task_slot,
                       u32 deadline) {
    struct kernel_object *objects[1] = {object};
    u32 ready = 0;
    return event_wait_many(objects, 1, task_slot, deadline, 1, &ready);
}

void event_tick(u32 now, i64 timeout_result) {
    for (u32 slot = 0; slot < (u32)task_pool_count; slot++) {
        if (!wait_timed[slot] ||
            (i32)(now - wait_deadlines[slot]) < 0)
            continue;
        wake_slot(slot, timeout_result);
    }
}

int event_signal(struct kernel_object *object) {
    irq_state_t irq = irq_save();
    struct event_state *state = state_for(object);
    if (!state) {
        irq_restore(irq);
        return -1;
    }
    if (!state->waiters) {
        state->signaled = 1;
        irq_restore(irq);
        return 0;
    }
    if (state->mode == EVENT_MANUAL_RESET) {
        u32 waiters = state->waiters;
        state->waiters = 0;
        state->signaled = 1;
        for (u32 slot = 0; slot < MAX_TASKS; slot++)
            if (waiters & (1u << slot))
                wake_slot(slot, wait_index(slot, object));
        irq_restore(irq);
        return 0;
    }
    for (u32 slot = 0; slot < MAX_TASKS; slot++) {
        u32 bit = 1u << slot;
        if (!(state->waiters & bit)) continue;
        i64 result = wait_index(slot, object);
        state->waiters &= ~bit;
        wake_slot(slot, result);
        irq_restore(irq);
        return 0;
    }
    irq_restore(irq);
    return -1;
}

int event_reset(struct kernel_object *object) {
    irq_state_t irq = irq_save();
    struct event_state *state = state_for(object);
    if (!state) {
        irq_restore(irq);
        return -1;
    }
    state->signaled = 0;
    irq_restore(irq);
    return 0;
}

void event_cancel_task(u32 task_slot, i64 result) {
    irq_state_t irq = irq_save();
    if (task_slot < MAX_TASKS) {
        if (task_pool[task_slot].state == TASK_BLOCKED_EVENT)
            wake_slot(task_slot, result);
        else
            clear_wait_set(task_slot);
    }
    irq_restore(irq);
}
