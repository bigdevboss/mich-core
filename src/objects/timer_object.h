#ifndef TIMER_OBJECT_H
#define TIMER_OBJECT_H

#include "types.h"
#include "object.h"

#define TIMER_OBJECT_MAX 32

void timer_object_init(void);
struct kernel_object *timer_object_create(void);
int timer_object_arm(struct kernel_object *object, u32 deadline, u32 interval);
int timer_object_cancel(struct kernel_object *object);
struct kernel_object *timer_object_wait_event(struct kernel_object *object);
void timer_object_tick(u32 now);
u32 timer_object_active_count(void);

#endif
