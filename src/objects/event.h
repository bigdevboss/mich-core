#ifndef EVENT_H
#define EVENT_H

#include "types.h"
#include "object.h"

#define EVENT_MAX 64
#define EVENT_WAIT_MANY_MAX 16

#define EVENT_AUTO_RESET 0
#define EVENT_MANUAL_RESET 1

typedef void (*event_wake_fn)(u32 task_slot, i64 result);

void event_init(event_wake_fn wake);
struct kernel_object *event_create(u32 mode, int signaled);
int event_wait(struct kernel_object *object, u32 task_slot);
int event_wait_timeout(struct kernel_object *object, u32 task_slot,
                       u32 deadline);
int event_wait_many(struct kernel_object **objects, u32 count, u32 task_slot,
                    u32 deadline, int timed, u32 *ready_index);
void event_tick(u32 now, i64 timeout_result);
int event_signal(struct kernel_object *object);
int event_reset(struct kernel_object *object);
void event_cancel_task(u32 task_slot, i64 result);

#endif
