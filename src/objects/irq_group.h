#ifndef IRQ_GROUP_H
#define IRQ_GROUP_H

#include "types.h"

#define IRQ_GROUP_MAX 32

struct irq_group_request {
    u32 resource_handle;
    u32 first;
    u32 count;
    u32 reserved;
    u32 handles[IRQ_GROUP_MAX];
};

#endif
