#ifndef COMPLETION_H
#define COMPLETION_H

#include "types.h"
#include "object.h"

#define COMPLETION_MAX 32
#define COMPLETION_REQUEST_MAX 32

#define COMPLETION_FREE 0
#define COMPLETION_PENDING 1
#define COMPLETION_DONE 2
#define COMPLETION_CANCELED 3
#define COMPLETION_TIMED_OUT 4

struct completion_result {
    u64 id;
    i32 status;
    u32 transferred;
    u32 state;
};

void completion_init(void);
struct kernel_object *completion_create(void);
u64 completion_request_begin(struct kernel_object *object, u32 deadline,
                             int timed);
int completion_request_finish(struct kernel_object *object, u64 id,
                              i32 status, u32 transferred);
int completion_request_cancel(struct kernel_object *object, u64 id);
int completion_request_poll(struct kernel_object *object, u64 id,
                            struct completion_result *result);
struct kernel_object *completion_wait_event(struct kernel_object *object);
void completion_tick(u32 now, i32 timeout_status);
u32 completion_active_count(void);

#endif
