#ifndef HANDLE_TRANSFER_H
#define HANDLE_TRANSFER_H

#include "types.h"

#define HANDLE_TRANSFER_MAX 16

struct handle_transfer_entry {
    u32 source_handle;
    u32 rights;
    u32 target_handle;
};

#endif
