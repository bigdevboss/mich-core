#ifndef FIRMWARE_H
#define FIRMWARE_H

#include "types.h"
#include "object.h"

struct kernel_object *firmware_open(const char *name, u32 *size);

#endif
