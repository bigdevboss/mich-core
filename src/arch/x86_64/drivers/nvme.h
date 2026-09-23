#ifndef NVME_H
#define NVME_H

#include "types.h"

#define NVME_DEVICE_MAX 4
#define NVME_DATA_SLOT_MAX 16
#define NVME_SGL_ENTRY_MAX 32

struct kernel_object;

struct kernel_object *nvme_open(struct kernel_object *pci);
u32 nvme_active_count(void);

#endif
