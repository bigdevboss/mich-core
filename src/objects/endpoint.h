#ifndef ENDPOINT_H
#define ENDPOINT_H

#include "types.h"
#include "object.h"

#define ENDPOINT_MAX 64

typedef int (*endpoint_notify_fn)(void *context, u32 source);
typedef void (*endpoint_cleanup_fn)(void *context);

void endpoint_init(void);
struct kernel_object *endpoint_create(endpoint_notify_fn notify,
                                      void *context);
struct kernel_object *endpoint_create_ex(endpoint_notify_fn notify,
                                         void *context,
                                         endpoint_cleanup_fn cleanup);
int endpoint_signal(struct kernel_object *object, u32 source);
void *endpoint_context(struct kernel_object *object);

#endif
