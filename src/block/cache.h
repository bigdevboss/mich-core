#ifndef BLOCK_CACHE_H
#define BLOCK_CACHE_H

#include "types.h"
#include "object.h"

#define BLOCK_CACHE_MAX 16
#define BLOCK_CACHE_SECTORS 8

void block_cache_init(void);
int block_cache_read(struct kernel_object *device, u32 lba,
                     void *buffer, u32 sectors);
int block_cache_write(struct kernel_object *device, u32 lba,
                      const void *buffer, u32 sectors);
int block_cache_flush(struct kernel_object *device);
void block_cache_drop_device(u32 device);
u32 block_cache_dirty_count(void);

#endif
