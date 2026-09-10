#ifndef SG_H
#define SG_H

#include "types.h"

#define SG_USER_ENTRY_MAX 32

struct sg_user_entry {
    u32 page_handle;
    u32 page_index;
    u32 offset;
    u32 length;
};

struct sg_create_request {
    u32 entry_count;
    u32 reserved;
    struct sg_user_entry entries[SG_USER_ENTRY_MAX];
};

#endif
