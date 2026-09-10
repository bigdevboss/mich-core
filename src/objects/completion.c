#include "completion.h"
#include "event.h"

struct completion_request {
    u32 generation;
    u32 state;
    u32 deadline;
    u32 timed;
    i32 status;
    u32 transferred;
};

struct completion_state {
    struct kernel_object *event;
    struct completion_request requests[COMPLETION_REQUEST_MAX];
    u32 next_slot;
    u32 active;
};

static struct completion_state completions[COMPLETION_MAX];

static struct completion_state *state_for(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_COMPLETION ||
        !object->value || object->value > COMPLETION_MAX)
        return 0;
    struct completion_state *state = &completions[object->value - 1];
    return state->active ? state : 0;
}

static u64 request_id(const struct completion_request *request, u32 slot) {
    return ((u64)request->generation << 32) | (slot + 1);
}

static struct completion_request *request_for(struct completion_state *state,
                                               u64 id, u32 *slot_out) {
    u32 low = (u32)id;
    u32 generation = (u32)(id >> 32);
    if (!low || low > COMPLETION_REQUEST_MAX || !generation) return 0;
    u32 slot = low - 1;
    struct completion_request *request = &state->requests[slot];
    if (request->state == COMPLETION_FREE ||
        request->generation != generation)
        return 0;
    if (slot_out) *slot_out = slot;
    return request;
}

static void completion_destroy(struct kernel_object *object) {
    if (!object || !object->value || object->value > COMPLETION_MAX) return;
    struct completion_state *state = &completions[object->value - 1];
    if (!state->active) return;
    if (state->event) object_release(state->event);
    state->event = 0;
    for (u32 index = 0; index < COMPLETION_REQUEST_MAX; index++) {
        state->requests[index].generation++;
        if (!state->requests[index].generation)
            state->requests[index].generation = 1;
        state->requests[index].state = COMPLETION_FREE;
        state->requests[index].deadline = 0;
        state->requests[index].timed = 0;
        state->requests[index].status = 0;
        state->requests[index].transferred = 0;
    }
    state->next_slot = 0;
    state->active = 0;
}

void completion_init(void) {
    for (u32 index = 0; index < COMPLETION_MAX; index++) {
        completions[index].event = 0;
        completions[index].next_slot = 0;
        completions[index].active = 0;
        for (u32 slot = 0; slot < COMPLETION_REQUEST_MAX; slot++) {
            completions[index].requests[slot].generation = 1;
            completions[index].requests[slot].state = COMPLETION_FREE;
            completions[index].requests[slot].deadline = 0;
            completions[index].requests[slot].timed = 0;
            completions[index].requests[slot].status = 0;
            completions[index].requests[slot].transferred = 0;
        }
    }
}

struct kernel_object *completion_create(void) {
    for (u32 index = 0; index < COMPLETION_MAX; index++) {
        struct completion_state *state = &completions[index];
        if (state->active) continue;
        state->event = event_create(EVENT_AUTO_RESET, 0);
        if (!state->event) return 0;
        state->next_slot = 0;
        state->active = 1;
        struct kernel_object *object = object_create(
            KOBJECT_COMPLETION, index + 1, completion_destroy);
        if (!object) {
            object_release(state->event);
            state->event = 0;
            state->active = 0;
        }
        return object;
    }
    return 0;
}

u64 completion_request_begin(struct kernel_object *object, u32 deadline,
                             int timed) {
    struct completion_state *state = state_for(object);
    if (!state) return 0;
    for (u32 offset = 0; offset < COMPLETION_REQUEST_MAX; offset++) {
        u32 slot = (state->next_slot + offset) % COMPLETION_REQUEST_MAX;
        struct completion_request *request = &state->requests[slot];
        if (request->state != COMPLETION_FREE) continue;
        request->state = COMPLETION_PENDING;
        request->deadline = deadline;
        request->timed = timed != 0;
        request->status = 0;
        request->transferred = 0;
        state->next_slot = (slot + 1) % COMPLETION_REQUEST_MAX;
        return request_id(request, slot);
    }
    return 0;
}

static int finish_request(struct completion_state *state, u64 id, u32 new_state,
                          i32 status, u32 transferred) {
    struct completion_request *request = request_for(state, id, 0);
    if (!request || request->state != COMPLETION_PENDING) return -1;
    request->state = new_state;
    request->deadline = 0;
    request->timed = 0;
    request->status = status;
    request->transferred = transferred;
    return event_signal(state->event);
}

int completion_request_finish(struct kernel_object *object, u64 id,
                              i32 status, u32 transferred) {
    struct completion_state *state = state_for(object);
    return state ? finish_request(state, id, COMPLETION_DONE,
                                  status, transferred) : -1;
}

int completion_request_cancel(struct kernel_object *object, u64 id) {
    struct completion_state *state = state_for(object);
    return state ? finish_request(state, id, COMPLETION_CANCELED,
                                  -1, 0) : -1;
}

int completion_request_poll(struct kernel_object *object, u64 id,
                            struct completion_result *result) {
    struct completion_state *state = state_for(object);
    u32 slot;
    struct completion_request *request = state ? request_for(state, id, &slot) : 0;
    if (!request || !result || request->state == COMPLETION_PENDING) return -1;
    result->id = id;
    result->status = request->status;
    result->transferred = request->transferred;
    result->state = request->state;
    request->state = COMPLETION_FREE;
    request->deadline = 0;
    request->timed = 0;
    request->status = 0;
    request->transferred = 0;
    request->generation++;
    if (!request->generation) request->generation = 1;
    if (state->next_slot > slot) state->next_slot = slot;
    event_reset(state->event);
    for (u32 index = 0; index < COMPLETION_REQUEST_MAX; index++)
        if (state->requests[index].state != COMPLETION_FREE &&
            state->requests[index].state != COMPLETION_PENDING) {
            event_signal(state->event);
            break;
        }
    return 0;
}

struct kernel_object *completion_wait_event(struct kernel_object *object) {
    struct completion_state *state = state_for(object);
    return state ? state->event : 0;
}

void completion_tick(u32 now, i32 timeout_status) {
    for (u32 index = 0; index < COMPLETION_MAX; index++) {
        struct completion_state *state = &completions[index];
        if (!state->active) continue;
        for (u32 slot = 0; slot < COMPLETION_REQUEST_MAX; slot++) {
            struct completion_request *request = &state->requests[slot];
            if (request->state != COMPLETION_PENDING || !request->timed ||
                (i32)(now - request->deadline) < 0)
                continue;
            request->state = COMPLETION_TIMED_OUT;
            request->deadline = 0;
            request->timed = 0;
            request->status = timeout_status;
            request->transferred = 0;
            event_signal(state->event);
        }
    }
}

u32 completion_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < COMPLETION_MAX; index++)
        if (completions[index].active) count++;
    return count;
}
