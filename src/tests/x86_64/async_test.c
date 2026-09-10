#include "types.h"
#include "object.h"
#include "completion.h"
#include "timer_object.h"
#include "event.h"
#include "protos.h"
#include "tests64.h"

int test_async64(const struct test64_env *env) {
    u32 objects = object_active_count();
    u32 completions = completion_active_count();
    u32 timers = timer_object_active_count();
    struct kernel_object *completion = completion_create();
    struct kernel_object *timer = timer_object_create();
    struct kernel_object *event_one = event_create(EVENT_AUTO_RESET, 0);
    struct kernel_object *event_two = event_create(EVENT_AUTO_RESET, 0);
    if (!completion || !timer || !event_one || !event_two) {
        if (completion) object_release(completion);
        if (timer) object_release(timer);
        if (event_one) object_release(event_one);
        if (event_two) object_release(event_two);
        return -1;
    }
    u64 finished = completion_request_begin(completion, 0, 0);
    struct completion_result result;
    int valid = finished &&
        !completion_request_finish(completion, finished, 7, 99) &&
        !completion_request_poll(completion, finished, &result) &&
        result.id == finished && result.status == 7 &&
        result.transferred == 99 && result.state == COMPLETION_DONE &&
        completion_request_poll(completion, finished, &result) < 0;
    u64 canceled = completion_request_begin(completion, 0, 0);
    valid = valid && canceled && canceled != finished &&
        !completion_request_cancel(completion, canceled) &&
        !completion_request_poll(completion, canceled, &result) &&
        result.state == COMPLETION_CANCELED;
    u64 timed = completion_request_begin(completion, 10, 1);
    completion_tick(9, ETIMEDOUT);
    valid = valid && timed &&
        completion_request_poll(completion, timed, &result) < 0;
    completion_tick(10, ETIMEDOUT);
    valid = valid && !completion_request_poll(completion, timed, &result) &&
        result.state == COMPLETION_TIMED_OUT && result.status == ETIMEDOUT;
    env->target->state = TASK_RUNNING;
    u64 waiting = completion_request_begin(completion, 0, 0);
    struct kernel_object *completion_event = completion_wait_event(completion);
    valid = valid && waiting && completion_event &&
        event_wait(completion_event, env->target_slot) == 1 &&
        env->target->state == TASK_BLOCKED_EVENT &&
        !completion_request_finish(completion, waiting, 0, 1) &&
        env->target->state == TASK_RUNNING && *env->target_result == 0 &&
        !completion_request_poll(completion, waiting, &result);
    env->target->state = TASK_RUNNING;
    valid = valid && !timer_object_arm(timer, 20, 0) &&
        event_wait(timer_object_wait_event(timer), env->target_slot) == 1;
    timer_object_tick(19);
    valid = valid && env->target->state == TASK_BLOCKED_EVENT;
    timer_object_tick(20);
    valid = valid && env->target->state == TASK_RUNNING &&
        *env->target_result == 0;
    struct kernel_object *wait_set[2] = {event_one, event_two};
    u32 ready = 0;
    env->target->state = TASK_RUNNING;
    valid = valid && event_wait_many(wait_set, 2, env->target_slot, 40, 1, &ready) == 1 &&
        !event_signal(event_two) && env->target->state == TASK_RUNNING &&
        *env->target_result == 1 && !event_signal(event_one) &&
        !event_wait_many(wait_set, 2, env->target_slot, 0, 0, &ready) && ready == 0;
    env->target->state = TASK_RUNNING;
    valid = valid && event_wait_many(wait_set, 2, env->target_slot, 50, 1, &ready) == 1;
    event_tick(49, ETIMEDOUT);
    valid = valid && env->target->state == TASK_BLOCKED_EVENT;
    event_tick(50, ETIMEDOUT);
    valid = valid && env->target->state == TASK_RUNNING &&
        *env->target_result == (u64)(i64)ETIMEDOUT;
    u64 stale = completion_request_begin(completion, 0, 0);
    object_release(event_one);
    object_release(event_two);
    object_release(timer);
    object_release(completion);
    struct kernel_object *replacement = completion_create();
    u64 fresh = replacement ? completion_request_begin(replacement, 0, 0) : 0;
    valid = valid && stale && fresh && fresh != stale &&
        !completion_request_cancel(replacement, fresh) &&
        !completion_request_poll(replacement, fresh, &result);
    if (replacement) object_release(replacement);
    valid = valid && object_active_count() == objects &&
        completion_active_count() == completions &&
        timer_object_active_count() == timers;
    return valid ? 0 : -1;
}
