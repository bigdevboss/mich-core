#include "timer_object.h"
#include "event.h"

struct timer_object_state {
    struct kernel_object *event;
    u32 deadline;
    u32 interval;
    u32 armed;
    u32 active;
};

static struct timer_object_state timers[TIMER_OBJECT_MAX];

static struct timer_object_state *state_for(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_TIMER ||
        !object->value || object->value > TIMER_OBJECT_MAX)
        return 0;
    struct timer_object_state *state = &timers[object->value - 1];
    return state->active ? state : 0;
}

static void timer_destroy(struct kernel_object *object) {
    if (!object || !object->value || object->value > TIMER_OBJECT_MAX) return;
    struct timer_object_state *state = &timers[object->value - 1];
    if (!state->active) return;
    if (state->event) object_release(state->event);
    state->event = 0;
    state->deadline = 0;
    state->interval = 0;
    state->armed = 0;
    state->active = 0;
}

void timer_object_init(void) {
    for (u32 index = 0; index < TIMER_OBJECT_MAX; index++) {
        timers[index].event = 0;
        timers[index].deadline = 0;
        timers[index].interval = 0;
        timers[index].armed = 0;
        timers[index].active = 0;
    }
}

struct kernel_object *timer_object_create(void) {
    for (u32 index = 0; index < TIMER_OBJECT_MAX; index++) {
        struct timer_object_state *state = &timers[index];
        if (state->active) continue;
        state->event = event_create(EVENT_AUTO_RESET, 0);
        if (!state->event) return 0;
        state->deadline = 0;
        state->interval = 0;
        state->armed = 0;
        state->active = 1;
        struct kernel_object *object = object_create(
            KOBJECT_TIMER, index + 1, timer_destroy);
        if (!object) {
            object_release(state->event);
            state->event = 0;
            state->active = 0;
        }
        return object;
    }
    return 0;
}

int timer_object_arm(struct kernel_object *object, u32 deadline, u32 interval) {
    struct timer_object_state *state = state_for(object);
    if (!state || interval > 0x7FFFFFFFu) return -1;
    state->deadline = deadline;
    state->interval = interval;
    state->armed = 1;
    event_reset(state->event);
    return 0;
}

int timer_object_cancel(struct kernel_object *object) {
    struct timer_object_state *state = state_for(object);
    if (!state || !state->armed) return -1;
    state->deadline = 0;
    state->interval = 0;
    state->armed = 0;
    event_reset(state->event);
    return 0;
}

struct kernel_object *timer_object_wait_event(struct kernel_object *object) {
    struct timer_object_state *state = state_for(object);
    return state ? state->event : 0;
}

void timer_object_tick(u32 now) {
    for (u32 index = 0; index < TIMER_OBJECT_MAX; index++) {
        struct timer_object_state *state = &timers[index];
        if (!state->active || !state->armed ||
            (i32)(now - state->deadline) < 0)
            continue;
        event_signal(state->event);
        if (!state->interval) {
            state->armed = 0;
            state->deadline = 0;
        } else {
            do {
                state->deadline += state->interval;
            } while ((i32)(now - state->deadline) >= 0);
        }
    }
}

u32 timer_object_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < TIMER_OBJECT_MAX; index++)
        if (timers[index].active) count++;
    return count;
}
