#ifndef COMPLETION_ABI_H
#define COMPLETION_ABI_H

#include "types.h"

#define MICH_COMPLETION_DONE 2
#define MICH_COMPLETION_CANCELED 3
#define MICH_COMPLETION_TIMED_OUT 4

struct completion_update {
    u64 id;
    i32 status;
    u32 transferred;
};

struct completion_poll {
    u64 id;
    i32 status;
    u32 transferred;
    u32 state;
    u32 reserved;
};

#endif
