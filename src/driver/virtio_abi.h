#ifndef VIRTIO_ABI_H
#define VIRTIO_ABI_H

#include "types.h"

struct virtio_feature_request {
    u64 wanted;
    u64 required;
    u64 device_features;
    u64 driver_features;
};

#define VIRTIO_CONFIG_DATA_MAX 64
#define VIRTQUEUE_COMPLETION_BATCH_MAX 16

struct virtqueue_create_request {
    u32 device_handle;
    u16 queue_index;
    u16 queue_size;
    u32 queue_handle;
    u32 descriptor_offset;
    u32 available_offset;
    u32 used_offset;
    u32 total_bytes;
};

struct virtio_config_request {
    u32 offset;
    u32 length;
    u32 generation;
    u32 reserved;
    u8 data[VIRTIO_CONFIG_DATA_MAX];
};

struct virtqueue_chain_request {
    u32 queue_handle;
    u16 descriptor_count;
    u16 reserved;
    u64 token;
};

struct virtqueue_packet_request {
    u32 queue_handle;
    u32 interface_handle;
    u64 token;
    u64 buffer_id;
    u32 offset;
    u32 length;
    u16 ordinal;
    u16 writable;
};

struct virtqueue_completion_result {
    u32 queue_handle;
    u32 length;
    u64 token;
};

struct virtqueue_completion_item {
    u64 token;
    u32 length;
    u32 reserved;
};

struct virtqueue_completion_batch {
    u32 queue_handle;
    u32 maximum;
    u32 count;
    u32 reserved;
    struct virtqueue_completion_item items[VIRTQUEUE_COMPLETION_BATCH_MAX];
};

#endif
