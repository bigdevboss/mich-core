#ifndef BRIDGE_H
#define BRIDGE_H

#include "types.h"
#include "object.h"

#define BRIDGE_ENDPOINT_MAX 32
#define BRIDGE_QUEUE_MAX 64

struct bridge_notification {
    u32 source;
    u32 sequence;
};

void bridge_init(void);
struct kernel_object *bridge_endpoint_create(void);
int bridge_endpoint_wait(struct kernel_object *endpoint, u32 task_slot);
int bridge_endpoint_read(struct kernel_object *endpoint,
                         struct bridge_notification *notification);
struct kernel_object *bridge_endpoint_wait_event(struct kernel_object *endpoint);
u32 bridge_active_count(void);

#endif
