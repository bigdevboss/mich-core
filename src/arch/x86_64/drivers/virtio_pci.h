#ifndef VIRTIO_PCI_H
#define VIRTIO_PCI_H

#include "types.h"

#define VIRTIO_PCI_DEVICE_MAX 8
#define VIRTIO_QUEUE_MAX 32
#define VIRTIO_PCI_VENDOR 0x1AF4
#define VIRTIO_PCI_DEVICE_NET_LEGACY 0x1000
#define VIRTIO_PCI_DEVICE_NET 0x1041
#define VIRTIO_PCI_DEVICE_BLK 0x1042
#define VIRTIO_QUEUE_SIZE_MAX 256
#define VIRTIO_QUEUE_TOKEN_INVALID 0

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG 3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED 128

#define VIRTIO_FEATURE_VERSION_1 (1ULL << 32)
#define VIRTIO_NET_FEATURE_MAC (1ULL << 5)
#define VIRTIO_NET_FEATURE_STATUS (1ULL << 16)

#define VIRTQUEUE_DESC_NEXT 1
#define VIRTQUEUE_DESC_WRITE 2
#define VIRTQUEUE_USED_NO_NOTIFY 1
#define VIRTQUEUE_AVAILABLE_NO_INTERRUPT 1

#define VIRTQUEUE_CHAIN_FREE 0
#define VIRTQUEUE_CHAIN_ALLOCATED 1
#define VIRTQUEUE_CHAIN_PUBLISHED 2

struct kernel_object;

struct virtio_pci_region {
    u8 bar;
    u32 offset;
    u32 length;
    void *mapping;
    usize_t mapping_length;
    volatile u8 *address;
};

struct virtio_device_info {
    struct kernel_object *pci;
    struct virtio_pci_region common;
    struct virtio_pci_region notify;
    struct virtio_pci_region isr;
    struct virtio_pci_region device;
    u32 notify_multiplier;
    u64 device_features;
    u64 driver_features;
    u32 negotiated;
    u32 active;
};

struct virtqueue_descriptor {
    u64 address;
    u32 length;
    u16 flags;
    u16 next;
} __attribute__((packed));

struct virtqueue_used_element {
    u32 id;
    u32 length;
} __attribute__((packed));

struct virtqueue_completion {
    u64 token;
    u32 length;
};

struct virtqueue_info {
    struct kernel_object *device;
    struct kernel_object *dma;
    u16 queue_index;
    u16 queue_size;
    u16 notify_offset;
    u16 free_head;
    u16 free_count;
    u16 available_index;
    u16 used_index;
    u64 generation;
    u16 outstanding;
    u32 descriptor_offset;
    u32 available_offset;
    u32 used_offset;
    u32 total_bytes;
    u32 rejected_used;
    u32 notify_count;
    u32 suppressed_notify_count;
    u32 enabled;
    u32 failed;
    u32 active;
    u16 free_next[VIRTIO_QUEUE_SIZE_MAX];
    u16 chain_next[VIRTIO_QUEUE_SIZE_MAX];
    u16 chain_head[VIRTIO_QUEUE_SIZE_MAX];
    u16 chain_length[VIRTIO_QUEUE_SIZE_MAX];
    u64 chain_generation[VIRTIO_QUEUE_SIZE_MAX];
    u8 chain_state[VIRTIO_QUEUE_SIZE_MAX];
};

void virtio_pci_init(void);
struct kernel_object *virtio_pci_create(struct kernel_object *pci);
struct virtio_device_info *virtio_pci_get(struct kernel_object *object);
int virtio_pci_negotiate(struct kernel_object *object,
                         u64 wanted, u64 required);
int virtio_pci_set_driver_ok(struct kernel_object *object);
int virtio_pci_read_config(struct kernel_object *object, u32 offset,
                           void *buffer, u32 length, u32 *generation);
struct kernel_object *virtqueue_create(struct kernel_object *device,
                                       u16 queue_index, u16 requested_size);
struct virtqueue_info *virtqueue_get(struct kernel_object *object);
void *virtqueue_memory(struct kernel_object *object);
struct kernel_object *virtqueue_dma_object(struct kernel_object *object);
int virtqueue_chain_allocate(struct kernel_object *object, u16 count,
                             u64 *token);
int virtqueue_descriptor_set(struct kernel_object *object, u64 token,
                             u16 ordinal, u64 address, u32 length,
                             int writable);
int virtqueue_descriptor_set_packet(struct kernel_object *object, u64 token,
                                    u16 ordinal, struct kernel_object *pool,
                                    u64 buffer_id, u32 offset, u32 length,
                                    int writable);
int virtqueue_chain_release(struct kernel_object *object, u64 token);
int virtqueue_publish(struct kernel_object *object, u64 token);
int virtqueue_collect(struct kernel_object *object,
                      struct virtqueue_completion *completion);
int virtqueue_collect_batch(struct kernel_object *object,
                            struct virtqueue_completion *completions,
                            u32 maximum);
int virtqueue_reset_state(struct kernel_object *object);
int virtqueue_set_msix(struct kernel_object *object,
                       struct kernel_object *irq);
int virtqueue_notify(struct kernel_object *object);
u32 virtio_pci_active_count(void);
u32 virtqueue_active_count(void);

#endif
