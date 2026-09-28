#ifndef VIRTIO_LIB_H
#define VIRTIO_LIB_H

// Userspace virtio-pci transport, statically linked into each virtio capsule so
// the kernel keeps no in-kernel virtio driver. It drives a modern (VERSION_1)
// device over raw hardware handles alone: PCI config reads, BAR mmio mappings
// and a manifest-granted DMA region for the split virtqueue. The ring
// bookkeeping mirrors the former in-kernel transport (virtio spec 4.1, 2.7) but
// needs no locking, since a capsule is the single-threaded owner of its queues.

#define VIRTIO_LIB_QUEUE_SIZE_MAX 256u

// Feature bit that marks a non-legacy device; the transport refuses anything
// that cannot offer it (virtio spec 1.1 section 6).
#define VIRTIO_FEATURE_VERSION_1 (1ULL << 32)

#define VIRTQUEUE_DESC_NEXT 1u
#define VIRTQUEUE_DESC_WRITE 2u

// virtio_device_setup records the failing step here before it returns nonzero, so
// a capsule can report the precise bring-up fault instead of a bare failure.
#define VIRTIO_SETUP_OK 0u
#define VIRTIO_SETUP_NO_CAPABILITIES 1u
#define VIRTIO_SETUP_BAD_CAPABILITY 2u
#define VIRTIO_SETUP_BAR_OPEN 3u
#define VIRTIO_SETUP_BAR_TOO_LARGE 4u
#define VIRTIO_SETUP_BAR_MAP 5u
#define VIRTIO_SETUP_REGION_RANGE 6u
#define VIRTIO_SETUP_MISSING_REGION 7u
#define VIRTIO_SETUP_COMMAND 8u

struct virtio_region {
    volatile unsigned char *address;
    unsigned int length;
};

struct virtio_device {
    unsigned int pci_handle;
    unsigned long long bar_window_base;
    unsigned long long mapped_bars;
    struct virtio_region common;
    struct virtio_region notify;
    struct virtio_region isr;
    struct virtio_region device;
    unsigned int notify_multiplier;
    unsigned long long driver_features;
    unsigned int setup_error;
};

// The chain bookkeeping is indexed by descriptor slot, so the arrays are sized to
// the transport's maximum queue and the live queue uses a prefix of them. A token
// packs the chain-head slot in its low 16 bits and a monotonic generation above,
// so a stale token from an already-collected chain cannot be replayed.
struct virtqueue {
    struct virtio_device *device;
    volatile unsigned char *base;
    unsigned long long physical;
    unsigned int queue_index;
    unsigned int queue_size;
    unsigned int notify_offset;
    unsigned int descriptor_offset;
    unsigned int available_offset;
    unsigned int used_offset;
    unsigned int available_index;
    unsigned int used_index;
    unsigned int free_head;
    unsigned int free_count;
    unsigned long long generation;
    unsigned short free_next[VIRTIO_LIB_QUEUE_SIZE_MAX];
    unsigned short chain_next[VIRTIO_LIB_QUEUE_SIZE_MAX];
    unsigned short chain_head[VIRTIO_LIB_QUEUE_SIZE_MAX];
    unsigned short chain_length[VIRTIO_LIB_QUEUE_SIZE_MAX];
    unsigned long long chain_generation[VIRTIO_LIB_QUEUE_SIZE_MAX];
    unsigned char chain_state[VIRTIO_LIB_QUEUE_SIZE_MAX];
};

// Walk the PCI capability list, map the common/notify/isr/device regions from
// whichever BARs they name into bar_window_base, and enable bus mastering.
int virtio_device_setup(struct virtio_device *device, unsigned int pci_handle,
                        unsigned long long bar_window_base);

// Reset the device, read its features, and write back the wanted subset. Fails
// closed unless every required bit (and VERSION_1) survives negotiation.
int virtio_negotiate(struct virtio_device *device, unsigned long long wanted,
                     unsigned long long required);

int virtio_read_config(struct virtio_device *device, unsigned int offset,
                       unsigned char *buffer, unsigned int length);

int virtio_driver_ok(struct virtio_device *device);

// Size and enable one split virtqueue in the caller-provided DMA region already
// mapped at vring_vaddr with guest-physical base vring_physical.
int virtqueue_setup(struct virtio_device *device, struct virtqueue *queue,
                    unsigned int queue_index, unsigned int requested_size,
                    unsigned long long vring_vaddr,
                    unsigned long long vring_physical,
                    unsigned long long vring_length);

int virtqueue_chain_alloc(struct virtqueue *queue, unsigned int count,
                          unsigned long long *token);

int virtqueue_descriptor_set(struct virtqueue *queue, unsigned long long token,
                             unsigned int ordinal, unsigned long long address,
                             unsigned int length, int writable);

int virtqueue_chain_publish(struct virtqueue *queue, unsigned long long token);

int virtqueue_chain_release(struct virtqueue *queue, unsigned long long token);

// Returns 1 and fills token/length when the device retired a chain, 0 when the
// used ring is empty, negative when the device reported an inconsistent entry.
int virtqueue_collect(struct virtqueue *queue, unsigned long long *token,
                      unsigned int *length);

int virtqueue_kick(struct virtqueue *queue);

#endif
