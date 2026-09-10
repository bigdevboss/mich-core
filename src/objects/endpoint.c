#include "endpoint.h"

struct endpoint_state {
    endpoint_notify_fn notify;
    endpoint_cleanup_fn cleanup;
    void *context;
    u32 active;
};

static struct endpoint_state endpoints[ENDPOINT_MAX];

static struct endpoint_state *state_for(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_ENDPOINT ||
        !object->value || object->value > ENDPOINT_MAX)
        return 0;
    struct endpoint_state *state = &endpoints[object->value - 1];
    return state->active ? state : 0;
}

static void endpoint_destroy(struct kernel_object *object) {
    if (!object || object->type != KOBJECT_ENDPOINT ||
        !object->value || object->value > ENDPOINT_MAX)
        return;
    struct endpoint_state *state = &endpoints[object->value - 1];
    if (state->cleanup) state->cleanup(state->context);
    state->notify = 0;
    state->cleanup = 0;
    state->context = 0;
    state->active = 0;
}

void endpoint_init(void) {
    for (u32 index = 0; index < ENDPOINT_MAX; index++) {
        endpoints[index].notify = 0;
        endpoints[index].cleanup = 0;
        endpoints[index].context = 0;
        endpoints[index].active = 0;
    }
}

struct kernel_object *endpoint_create_ex(endpoint_notify_fn notify,
                                         void *context,
                                         endpoint_cleanup_fn cleanup) {
    if (!notify) return 0;
    for (u32 index = 0; index < ENDPOINT_MAX; index++) {
        struct endpoint_state *state = &endpoints[index];
        if (state->active) continue;
        state->notify = notify;
        state->cleanup = cleanup;
        state->context = context;
        state->active = 1;
        struct kernel_object *object =
            object_create(KOBJECT_ENDPOINT, index + 1, endpoint_destroy);
        if (!object) {
            state->notify = 0;
            state->cleanup = 0;
            state->context = 0;
            state->active = 0;
        }
        return object;
    }
    return 0;
}

struct kernel_object *endpoint_create(endpoint_notify_fn notify,
                                      void *context) {
    return endpoint_create_ex(notify, context, 0);
}

int endpoint_signal(struct kernel_object *object, u32 source) {
    struct endpoint_state *state = state_for(object);
    return state ? state->notify(state->context, source) : -1;
}

void *endpoint_context(struct kernel_object *object) {
    struct endpoint_state *state = state_for(object);
    return state ? state->context : 0;
}
