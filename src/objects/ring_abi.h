#ifndef RING_ABI_H
#define RING_ABI_H

#include "types.h"

#define RING_CAPACITY_MAX 128
#define RING_DESCRIPTOR_MIN 16
#define RING_DESCRIPTOR_MAX 256
#define RING_SHARED_HEADER_SIZE 64

struct ring_shared_header {
    u64 producer;
    u64 consumer;
    u32 generation;
    u32 capacity;
    u32 descriptor_size;
    u32 flags;
    u32 data_offset;
    u32 reserved0;
    u64 reserved[3];
};

typedef char ring_shared_header_size_check[
    sizeof(struct ring_shared_header) == RING_SHARED_HEADER_SIZE ? 1 : -1];

#endif
