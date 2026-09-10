#ifndef FIRMWARE_ABI_H
#define FIRMWARE_ABI_H

#include "types.h"

#define FIRMWARE_NAME_MAX 32

struct firmware_open_request {
    u32 file_handle;
    u32 size;
    u32 reserved0;
    u32 reserved1;
    char name[FIRMWARE_NAME_MAX];
};

#endif
