#ifndef BLOCK_H
#define BLOCK_H

#include "types.h"
#include "object.h"
#include "block_abi.h"

#define BLOCK_DEVICE_MAX 8
#define BLOCK_REQUEST_MAX 16

#define BLOCK_IO_SG_SECTORS_MAX 64

typedef int (*block_issue_fn)(struct kernel_object *queue,
                              struct kernel_object *dma, u32 slot, u32 op,
                              u32 lba, const u8 *data, u64 *token);
typedef int (*block_issue_sg_fn)(struct kernel_object *queue,
                                 struct kernel_object *dma, u32 slot,
                                 u32 op, u32 lba,
                                 struct kernel_object *sg, u64 *token);
typedef int (*block_reap_fn)(struct kernel_object *queue,
                             struct kernel_object *dma, const u64 *tokens,
                             u32 n, u32 *slot, i32 *status, u8 *data);

void block_init(void);
struct kernel_object *block_create(u32 sector_count, u32 flags);
struct kernel_object *block_bind_transport(u32 sector_count, u32 flags,
    struct kernel_object *transport, struct kernel_object *queue,
    struct kernel_object *dma, block_issue_fn issue, block_reap_fn reap,
    block_issue_sg_fn issue_sg);
int block_info(struct kernel_object *object, struct block_info *info);
int block_submit(struct kernel_object *object, u32 op, u32 lba, u32 sectors,
                 void *buffer, u32 length, u64 *id);
int block_submit_sg(struct kernel_object *object, u32 op, u32 lba,
                     u32 sectors, struct kernel_object *sg, u64 *id);
int block_service(struct kernel_object *object);
int block_collect(struct kernel_object *object, u64 id,
                  i32 *status, u32 *transferred, void *buffer, u32 length);
int block_io(struct kernel_object *object, u32 op, u32 lba, u32 sectors,
             void *buffer, u32 length);
int block_revoke(struct kernel_object *object);
struct kernel_object *block_wait_event(struct kernel_object *object);
u32 block_active_count(void);

#endif
