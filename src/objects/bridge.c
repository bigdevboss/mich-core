#include "bridge.h"
#include "endpoint.h"
#include "event.h"
#include "irq.h"

struct bridge_state {
    struct kernel_object *event;
    struct bridge_notification queue[BRIDGE_QUEUE_MAX];
    u32 head;
    u32 count;
    u32 sequence;
    u32 active;
};

static struct bridge_state bridges[BRIDGE_ENDPOINT_MAX];

static struct bridge_state *state_from_context(void *context) {
    uptr_t address = (uptr_t)context;
    uptr_t base = (uptr_t)bridges;
    if (!context || address < base || address >= base + sizeof(bridges) ||
        (address - base) % sizeof(struct bridge_state))
        return 0;
    struct bridge_state *state = (struct bridge_state *)context;
    return state->active ? state : 0;
}

static struct bridge_state *state_for(struct kernel_object *endpoint) {
    if (!endpoint || endpoint->type != KOBJECT_ENDPOINT) return 0;
    return state_from_context(endpoint_context(endpoint));
}

static int bridge_notify(void *context, u32 source) {
    irq_state_t irq = irq_save();
    struct bridge_state *state = state_from_context(context);
    if (!state || state->count >= BRIDGE_QUEUE_MAX) {
        irq_restore(irq);
        return -1;
    }
    u32 tail = (state->head + state->count) % BRIDGE_QUEUE_MAX;
    state->queue[tail].source = source;
    state->queue[tail].sequence = state->sequence++;
    state->count++;
    int result = event_signal(state->event);
    irq_restore(irq);
    return result;
}

static void bridge_cleanup(void *context) {
    struct bridge_state *state = state_from_context(context);
    if (!state) return;
    struct kernel_object *event = state->event;
    state->event = 0;
    state->head = 0;
    state->count = 0;
    state->sequence = 0;
    state->active = 0;
    if (event) object_release(event);
}

void bridge_init(void) {
    for (u32 index = 0; index < BRIDGE_ENDPOINT_MAX; index++) {
        bridges[index].event = 0;
        bridges[index].head = 0;
        bridges[index].count = 0;
        bridges[index].sequence = 0;
        bridges[index].active = 0;
    }
}

struct kernel_object *bridge_endpoint_create(void) {
    for (u32 index = 0; index < BRIDGE_ENDPOINT_MAX; index++) {
        struct bridge_state *state = &bridges[index];
        if (state->active) continue;
        state->event = event_create(EVENT_AUTO_RESET, 0);
        if (!state->event) return 0;
        state->head = 0;
        state->count = 0;
        state->sequence = 1;
        state->active = 1;
        struct kernel_object *endpoint =
            endpoint_create_ex(bridge_notify, state, bridge_cleanup);
        if (!endpoint) bridge_cleanup(state);
        return endpoint;
    }
    return 0;
}

int bridge_endpoint_wait(struct kernel_object *endpoint, u32 task_slot) {
    struct bridge_state *state = state_for(endpoint);
    return state ? event_wait(state->event, task_slot) : -1;
}

struct kernel_object *bridge_endpoint_wait_event(struct kernel_object *endpoint) {
    struct bridge_state *state = state_for(endpoint);
    return state ? state->event : 0;
}

int bridge_endpoint_read(struct kernel_object *endpoint,
                         struct bridge_notification *notification) {
    irq_state_t irq = irq_save();
    struct bridge_state *state = state_for(endpoint);
    if (!state || !notification || !state->count) {
        irq_restore(irq);
        return -1;
    }
    *notification = state->queue[state->head];
    state->head = (state->head + 1) % BRIDGE_QUEUE_MAX;
    state->count--;
    if (state->count) event_signal(state->event);
    else event_reset(state->event);
    irq_restore(irq);
    return 0;
}

u32 bridge_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < BRIDGE_ENDPOINT_MAX; index++)
        if (bridges[index].active) count++;
    return count;
}
