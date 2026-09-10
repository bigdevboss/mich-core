#ifndef MICH64_USER_VIRTIO_H
#define MICH64_USER_VIRTIO_H

#include <virtio_abi.h>

#define MICH_VIRTIO_FEATURE_VERSION_1 (1ULL << 32)
#define MICH_VIRTIO_NET_FEATURE_MAC (1ULL << 5)
#define MICH_VIRTIO_NET_FEATURE_STATUS (1ULL << 16)

#define MICH_VIRTIO_CONFIG_DATA_MAX VIRTIO_CONFIG_DATA_MAX
#define MICH_VIRTQUEUE_COMPLETION_BATCH_MAX VIRTQUEUE_COMPLETION_BATCH_MAX

#define mich_virtio_feature_request virtio_feature_request
#define mich_virtqueue_create_request virtqueue_create_request
#define mich_virtio_config_request virtio_config_request
#define mich_virtqueue_chain_request virtqueue_chain_request
#define mich_virtqueue_packet_request virtqueue_packet_request
#define mich_virtqueue_completion_result virtqueue_completion_result
#define mich_virtqueue_completion_item virtqueue_completion_item
#define mich_virtqueue_completion_batch virtqueue_completion_batch

int mich_virtio_open(unsigned int pci_handle);
int mich_virtio_negotiate(unsigned int handle,
                          struct mich_virtio_feature_request *request);
int mich_virtqueue_create(struct mich_virtqueue_create_request *request);
int mich_virtqueue_map(unsigned int handle,
                       unsigned long long virtual_address);
int mich_virtqueue_notify(unsigned int handle);
int mich_virtio_driver_ok(unsigned int handle);
int mich_virtio_read_config(unsigned int handle,
                            struct mich_virtio_config_request *request);
int mich_virtqueue_chain_allocate(struct mich_virtqueue_chain_request *request);
int mich_virtqueue_set_packet(struct mich_virtqueue_packet_request *request);
int mich_virtqueue_publish(struct mich_virtqueue_chain_request *request);
int mich_virtqueue_collect(struct mich_virtqueue_completion_result *result);
int mich_virtqueue_chain_release(struct mich_virtqueue_chain_request *request);
int mich_virtqueue_set_msix(unsigned int queue_handle,
                            unsigned int irq_handle);
int mich_virtqueue_collect_batch(
    struct mich_virtqueue_completion_batch *batch);

#endif
