#ifndef WAIT_MANY_H
#define WAIT_MANY_H

#include "types.h"

#define WAIT_MANY_MAX 16

struct wait_many_request {
    u32 count;
    u32 timeout;
    u32 handles[WAIT_MANY_MAX];
};

#endif
