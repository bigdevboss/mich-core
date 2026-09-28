#include <mich/hardware.h>

#include "virtio.h"

// PCI vendor-specific capability that carries virtio structure locators, and the
// cfg_type values that name each locator (virtio spec 4.1.4).
#define VIRTIO_CAP_VENDOR 0x09
#define VIRTIO_CAP_COMMON 1
#define VIRTIO_CAP_NOTIFY 2
#define VIRTIO_CAP_ISR 3
#define VIRTIO_CAP_DEVICE 4

#define VIRTIO_STATUS_ACKNOWLEDGE 1u
#define VIRTIO_STATUS_DRIVER 2u
#define VIRTIO_STATUS_DRIVER_OK 4u
#define VIRTIO_STATUS_FEATURES_OK 8u
#define VIRTIO_STATUS_FAILED 128u

// Common-configuration register byte offsets (virtio spec 4.1.4.3).
#define VIRTIO_COMMON_DEVICE_FEATURE_SELECT 0u
#define VIRTIO_COMMON_DEVICE_FEATURE 4u
#define VIRTIO_COMMON_DRIVER_FEATURE_SELECT 8u
#define VIRTIO_COMMON_DRIVER_FEATURE 12u
#define VIRTIO_COMMON_DEVICE_STATUS 20u
#define VIRTIO_COMMON_CONFIG_GENERATION 21u
#define VIRTIO_COMMON_QUEUE_SELECT 22u
#define VIRTIO_COMMON_QUEUE_SIZE 24u
#define VIRTIO_COMMON_QUEUE_ENABLE 28u
#define VIRTIO_COMMON_QUEUE_NOTIFY_OFF 30u
#define VIRTIO_COMMON_QUEUE_DESC 32u
#define VIRTIO_COMMON_QUEUE_DRIVER 40u
#define VIRTIO_COMMON_QUEUE_DEVICE 48u

#define VIRTQUEUE_USED_NO_NOTIFY 1u

#define CHAIN_FREE 0u
#define CHAIN_ALLOCATED 1u
#define CHAIN_PUBLISHED 2u

// Each BAR the caps reference is mapped at a fixed stride inside the capsule's
// window, so a region address is its window slot plus the cap offset. The stride
// bounds any single BAR; a larger one is refused rather than aliased.
#define VIRTIO_BAR_STRIDE 0x100000ULL

struct vring_desc {
    unsigned long long address;
    unsigned int length;
    unsigned short flags;
    unsigned short next;
} __attribute__((packed));

struct vring_used_element {
    unsigned int id;
    unsigned int length;
} __attribute__((packed));

static unsigned short read16(volatile unsigned char *base, unsigned int off) {
    return *(volatile unsigned short *)(base + off);
}

static unsigned int read32(volatile unsigned char *base, unsigned int off) {
    return *(volatile unsigned int *)(base + off);
}

static void write16(volatile unsigned char *base, unsigned int off,
                    unsigned short value) {
    *(volatile unsigned short *)(base + off) = value;
}

static void write32(volatile unsigned char *base, unsigned int off,
                    unsigned int value) {
    *(volatile unsigned int *)(base + off) = value;
}

static void write64(volatile unsigned char *base, unsigned int off,
                    unsigned long long value) {
    *(volatile unsigned int *)(base + off) = (unsigned int)value;
    *(volatile unsigned int *)(base + off + 4) = (unsigned int)(value >> 32);
}

static volatile struct vring_desc *descriptors(struct virtqueue *queue) {
    return (volatile struct vring_desc *)(queue->base + queue->descriptor_offset);
}

static int map_region(struct virtio_device *device, unsigned int bar,
                      unsigned int offset, unsigned int length,
                      long long *bar_length, struct virtio_region *region) {
    if (bar >= 6u || !length) {
        device->setup_error = VIRTIO_SETUP_BAD_CAPABILITY;
        return -1;
    }
    device->setup_bar = bar;
    unsigned long long window =
        device->bar_window_base + (unsigned long long)bar * VIRTIO_BAR_STRIDE;
    if (!(device->mapped_bars & (1ULL << bar))) {
        int handle = mich_pci_bar_open(device->pci_handle, bar);
        if (handle <= 0) {
            device->setup_error = VIRTIO_SETUP_BAR_OPEN;
            return -1;
        }
        long long resource_length = mich_resource_length((unsigned int)handle);
        if (resource_length <= 0 ||
            (unsigned long long)resource_length > VIRTIO_BAR_STRIDE) {
            device->setup_error = VIRTIO_SETUP_BAR_TOO_LARGE;
            return -1;
        }
        if (mich_mmio_map((unsigned int)handle, window)) {
            device->setup_error = VIRTIO_SETUP_BAR_MAP;
            return -1;
        }
        bar_length[bar] = resource_length;
        device->mapped_bars |= (1ULL << bar);
    }
    if (bar_length[bar] < 0 || offset > (unsigned long long)bar_length[bar] ||
        length > (unsigned long long)bar_length[bar] - offset) {
        device->setup_error = VIRTIO_SETUP_REGION_RANGE;
        return -1;
    }
    region->address = (volatile unsigned char *)(window + offset);
    region->length = length;
    return 0;
}

int virtio_device_setup(struct virtio_device *device, unsigned int pci_handle,
                        unsigned long long bar_window_base) {
    unsigned char *bytes = (unsigned char *)device;
    for (unsigned int index = 0; index < sizeof(*device); index++)
        bytes[index] = 0;
    device->pci_handle = pci_handle;
    device->bar_window_base = bar_window_base;

    long status = mich_pci_config_read16(pci_handle, 0x06);
    long pointer = mich_pci_config_read8(pci_handle, 0x34);
    if (status < 0 || !((unsigned long)status & 0x10) || pointer < 0) {
        device->setup_error = VIRTIO_SETUP_NO_CAPABILITIES;
        return -1;
    }

    long long bar_length[6];
    for (unsigned int index = 0; index < 6u; index++) bar_length[index] = -1;

    unsigned char seen[32];
    for (unsigned int index = 0; index < sizeof(seen); index++) seen[index] = 0;
    unsigned int cursor = (unsigned int)pointer & 0xFFu;
    for (unsigned int depth = 0; cursor && depth < 48u; depth++) {
        if (cursor < 0x40u || cursor > 0xFCu || (cursor & 3u) ||
            (seen[cursor / 8u] & (unsigned char)(1u << (cursor % 8u)))) {
            device->setup_error = VIRTIO_SETUP_BAD_CAPABILITY;
            return -1;
        }
        seen[cursor / 8u] |= (unsigned char)(1u << (cursor % 8u));
        long id = mich_pci_config_read8(pci_handle, cursor);
        long next = mich_pci_config_read8(pci_handle, cursor + 1u);
        if (id < 0 || next < 0) {
            device->setup_error = VIRTIO_SETUP_BAD_CAPABILITY;
            return -1;
        }
        if (id == VIRTIO_CAP_VENDOR) {
            if (cursor > 0xECu) {
                device->setup_error = VIRTIO_SETUP_BAD_CAPABILITY;
                return -1;
            }
            long cap_length = mich_pci_config_read8(pci_handle, cursor + 2u);
            long cfg_type = mich_pci_config_read8(pci_handle, cursor + 3u);
            long bar = mich_pci_config_read8(pci_handle, cursor + 4u);
            long offset = mich_pci_config_read32(pci_handle, cursor + 8u);
            long length = mich_pci_config_read32(pci_handle, cursor + 12u);
            if (cap_length < 16 || cursor + (unsigned int)cap_length > 256u ||
                cfg_type < 0 || bar < 0 || offset < 0 || length < 0) {
                device->setup_error = VIRTIO_SETUP_BAD_CAPABILITY;
                return -1;
            }
            struct virtio_region *region = 0;
            if (cfg_type == VIRTIO_CAP_COMMON) region = &device->common;
            if (cfg_type == VIRTIO_CAP_NOTIFY) region = &device->notify;
            if (cfg_type == VIRTIO_CAP_ISR) region = &device->isr;
            if (cfg_type == VIRTIO_CAP_DEVICE) region = &device->device;
            if (region) {
                if (region->address) {
                    device->setup_error = VIRTIO_SETUP_BAD_CAPABILITY;
                    return -1;
                }
                if (map_region(device, (unsigned int)bar, (unsigned int)offset,
                               (unsigned int)length, bar_length, region))
                    return -1;
                if (cfg_type == VIRTIO_CAP_NOTIFY) {
                    long multiplier =
                        mich_pci_config_read32(pci_handle, cursor + 16u);
                    if (cap_length < 20 || multiplier < 0) {
                        device->setup_error = VIRTIO_SETUP_BAD_CAPABILITY;
                        return -1;
                    }
                    device->notify_multiplier = (unsigned int)multiplier;
                }
            }
        }
        if (next & 3) {
            device->setup_error = VIRTIO_SETUP_BAD_CAPABILITY;
            return -1;
        }
        cursor = (unsigned int)next;
    }
    if (!device->common.address || device->common.length < 56u ||
        !device->notify.address || device->notify.length < 2u ||
        !device->isr.address || device->isr.length < 1u) {
        device->setup_error = VIRTIO_SETUP_MISSING_REGION;
        return -1;
    }

    // Enable memory space and bus mastering so the device can reach the ring and
    // buffer physical addresses the capsule programs.
    long command = mich_pci_config_read16(pci_handle, 4);
    if (command < 0 ||
        mich_pci_set_command(pci_handle, 4, (unsigned int)command | 6u)) {
        device->setup_error = VIRTIO_SETUP_COMMAND;
        return -1;
    }
    return 0;
}

int virtio_negotiate(struct virtio_device *device, unsigned long long wanted,
                     unsigned long long required) {
    if ((required & ~wanted) || !(wanted & VIRTIO_FEATURE_VERSION_1))
        return -1;
    volatile unsigned char *common = device->common.address;
    common[VIRTIO_COMMON_DEVICE_STATUS] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    unsigned int spins = 0;
    while (common[VIRTIO_COMMON_DEVICE_STATUS] && spins++ < 1000000u)
        __asm__ volatile("pause");
    if (common[VIRTIO_COMMON_DEVICE_STATUS]) return -1;
    common[VIRTIO_COMMON_DEVICE_STATUS] =
        (unsigned char)(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    write32(common, VIRTIO_COMMON_DEVICE_FEATURE_SELECT, 0);
    unsigned long long features = read32(common, VIRTIO_COMMON_DEVICE_FEATURE);
    write32(common, VIRTIO_COMMON_DEVICE_FEATURE_SELECT, 1);
    features |= (unsigned long long)read32(common, VIRTIO_COMMON_DEVICE_FEATURE)
                << 32;
    if (!(features & VIRTIO_FEATURE_VERSION_1) || (required & ~features)) {
        common[VIRTIO_COMMON_DEVICE_STATUS] |= VIRTIO_STATUS_FAILED;
        return -1;
    }
    unsigned long long selected = features & wanted;
    write32(common, VIRTIO_COMMON_DRIVER_FEATURE_SELECT, 0);
    write32(common, VIRTIO_COMMON_DRIVER_FEATURE, (unsigned int)selected);
    write32(common, VIRTIO_COMMON_DRIVER_FEATURE_SELECT, 1);
    write32(common, VIRTIO_COMMON_DRIVER_FEATURE, (unsigned int)(selected >> 32));
    common[VIRTIO_COMMON_DEVICE_STATUS] |= VIRTIO_STATUS_FEATURES_OK;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!(common[VIRTIO_COMMON_DEVICE_STATUS] & VIRTIO_STATUS_FEATURES_OK)) {
        common[VIRTIO_COMMON_DEVICE_STATUS] |= VIRTIO_STATUS_FAILED;
        return -1;
    }
    device->driver_features = selected;
    return 0;
}

int virtio_read_config(struct virtio_device *device, unsigned int offset,
                       unsigned char *buffer, unsigned int length) {
    volatile unsigned char *common = device->common.address;
    if (!device->device.address || !length || offset > device->device.length ||
        length > device->device.length - offset)
        return -1;
    // Re-read until the config generation is stable across the copy, so a device
    // update mid-read cannot hand back a torn value (virtio spec 4.1.4.3.1).
    for (unsigned int attempt = 0; attempt < 8u; attempt++) {
        unsigned char before = common[VIRTIO_COMMON_CONFIG_GENERATION];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        for (unsigned int index = 0; index < length; index++)
            buffer[index] = device->device.address[offset + index];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (before == common[VIRTIO_COMMON_CONFIG_GENERATION]) return 0;
    }
    return -1;
}

int virtio_driver_ok(struct virtio_device *device) {
    volatile unsigned char *common = device->common.address;
    common[VIRTIO_COMMON_DEVICE_STATUS] |= VIRTIO_STATUS_DRIVER_OK;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return (common[VIRTIO_COMMON_DEVICE_STATUS] & VIRTIO_STATUS_DRIVER_OK) ?
        0 : -1;
}

int virtqueue_setup(struct virtio_device *device, struct virtqueue *queue,
                    unsigned int queue_index, unsigned int requested_size,
                    unsigned long long vring_vaddr,
                    unsigned long long vring_physical,
                    unsigned long long vring_length) {
    unsigned char *bytes = (unsigned char *)queue;
    for (unsigned int index = 0; index < sizeof(*queue); index++)
        bytes[index] = 0;
    volatile unsigned char *common = device->common.address;
    write16(common, VIRTIO_COMMON_QUEUE_SELECT, (unsigned short)queue_index);
    unsigned int maximum = read16(common, VIRTIO_COMMON_QUEUE_SIZE);
    if (!maximum) return -1;
    unsigned int size = requested_size ? requested_size : maximum;
    if (!size || size > maximum || size > VIRTIO_LIB_QUEUE_SIZE_MAX ||
        (size & (size - 1)))
        return -1;
    unsigned int available_offset = size * 16u;
    unsigned int used_offset = (available_offset + 6u + size * 2u + 3u) & ~3u;
    unsigned int total = used_offset + 6u + size * 8u;
    if ((unsigned long long)total > vring_length) return -1;
    unsigned int notify_offset = read16(common, VIRTIO_COMMON_QUEUE_NOTIFY_OFF);
    unsigned long long notify_byte =
        (unsigned long long)notify_offset * device->notify_multiplier;
    if (notify_byte > device->notify.length ||
        2u > device->notify.length - notify_byte)
        return -1;

    // The DMA region is granted once and reused across capsule restarts, so a
    // prior instance's used-ring index would otherwise be read as fresh
    // completions before the device posts any. Clear it to a known baseline.
    volatile unsigned char *base = (volatile unsigned char *)vring_vaddr;
    for (unsigned int index = 0; index < total; index++) base[index] = 0;

    write16(common, VIRTIO_COMMON_QUEUE_SIZE, (unsigned short)size);
    write64(common, VIRTIO_COMMON_QUEUE_DESC, vring_physical);
    write64(common, VIRTIO_COMMON_QUEUE_DRIVER, vring_physical + available_offset);
    write64(common, VIRTIO_COMMON_QUEUE_DEVICE, vring_physical + used_offset);
    write16(common, VIRTIO_COMMON_QUEUE_ENABLE, 1);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!read16(common, VIRTIO_COMMON_QUEUE_ENABLE)) return -1;

    queue->device = device;
    queue->base = base;
    queue->physical = vring_physical;
    queue->queue_index = queue_index;
    queue->queue_size = size;
    queue->notify_offset = notify_offset;
    queue->available_offset = available_offset;
    queue->used_offset = used_offset;
    queue->free_count = size;
    for (unsigned int index = 0; index < size; index++) {
        queue->free_next[index] = (unsigned short)(index + 1u);
        queue->chain_next[index] = VIRTIO_LIB_QUEUE_SIZE_MAX;
        queue->chain_head[index] = VIRTIO_LIB_QUEUE_SIZE_MAX;
    }
    queue->free_next[size - 1u] = VIRTIO_LIB_QUEUE_SIZE_MAX;
    return 0;
}

static int token_head(struct virtqueue *queue, unsigned long long token,
                      unsigned char required_state, unsigned int *head) {
    unsigned int index = (unsigned int)(token & 0xFFFFu);
    unsigned long long generation = token >> 16;
    if (!token || index >= queue->queue_size || !generation ||
        queue->chain_head[index] != index ||
        queue->chain_generation[index] != generation ||
        queue->chain_state[index] != required_state)
        return -1;
    *head = index;
    return 0;
}

static void release_chain(struct virtqueue *queue, unsigned int head) {
    unsigned int count = queue->chain_length[head];
    unsigned int index = head;
    for (unsigned int ordinal = 0; ordinal < count; ordinal++) {
        unsigned int next = queue->chain_next[index];
        queue->chain_next[index] = VIRTIO_LIB_QUEUE_SIZE_MAX;
        queue->chain_head[index] = VIRTIO_LIB_QUEUE_SIZE_MAX;
        queue->chain_length[index] = 0;
        queue->chain_generation[index] = 0;
        queue->chain_state[index] = CHAIN_FREE;
        queue->free_next[index] = (unsigned short)queue->free_head;
        queue->free_head = index;
        queue->free_count++;
        index = next;
    }
}

int virtqueue_chain_alloc(struct virtqueue *queue, unsigned int count,
                          unsigned long long *token) {
    if (!token || !count || count > queue->queue_size ||
        count > queue->free_count)
        return -1;
    queue->generation++;
    if (!queue->generation || queue->generation > 0xFFFFFFFFFFFFULL)
        queue->generation = 1;
    unsigned long long generation = queue->generation;
    unsigned int head = queue->free_head;
    unsigned int index = head;
    volatile struct vring_desc *desc = descriptors(queue);
    for (unsigned int ordinal = 0; ordinal < count; ordinal++) {
        unsigned int next = queue->free_next[index];
        int more = ordinal + 1u < count;
        queue->free_head = next;
        queue->free_count--;
        queue->chain_head[index] = (unsigned short)head;
        queue->chain_generation[index] = generation;
        queue->chain_state[index] = CHAIN_ALLOCATED;
        queue->chain_next[index] =
            (unsigned short)(more ? next : VIRTIO_LIB_QUEUE_SIZE_MAX);
        desc[index].address = 0;
        desc[index].length = 0;
        desc[index].flags = (unsigned short)(more ? VIRTQUEUE_DESC_NEXT : 0u);
        desc[index].next = (unsigned short)(more ? next : 0u);
        index = next;
    }
    queue->chain_length[head] = (unsigned short)count;
    *token = (generation << 16) | head;
    return 0;
}

int virtqueue_descriptor_set(struct virtqueue *queue, unsigned long long token,
                             unsigned int ordinal, unsigned long long address,
                             unsigned int length, int writable) {
    if (!length || address > ~0ULL - (length - 1u)) return -1;
    unsigned int head;
    if (token_head(queue, token, CHAIN_ALLOCATED, &head) ||
        ordinal >= queue->chain_length[head])
        return -1;
    unsigned int index = head;
    for (unsigned int current = 0; current < ordinal; current++)
        index = queue->chain_next[index];
    volatile struct vring_desc *desc = descriptors(queue);
    unsigned short flags = desc[index].flags & VIRTQUEUE_DESC_NEXT;
    if (writable) flags |= VIRTQUEUE_DESC_WRITE;
    desc[index].address = address;
    desc[index].length = length;
    desc[index].flags = flags;
    return 0;
}

int virtqueue_chain_release(struct virtqueue *queue, unsigned long long token) {
    unsigned int head;
    if (token_head(queue, token, CHAIN_ALLOCATED, &head)) return -1;
    release_chain(queue, head);
    return 0;
}

int virtqueue_chain_publish(struct virtqueue *queue, unsigned long long token) {
    unsigned int head;
    if (token_head(queue, token, CHAIN_ALLOCATED, &head)) return -1;
    volatile struct vring_desc *desc = descriptors(queue);
    unsigned int index = head;
    for (unsigned int ordinal = 0; ordinal < queue->chain_length[head];
         ordinal++) {
        if (!desc[index].length) return -1;
        index = queue->chain_next[index];
    }
    index = head;
    for (unsigned int ordinal = 0; ordinal < queue->chain_length[head];
         ordinal++) {
        queue->chain_state[index] = CHAIN_PUBLISHED;
        index = queue->chain_next[index];
    }
    volatile unsigned short *available =
        (volatile unsigned short *)(queue->base + queue->available_offset);
    available[2u + (queue->available_index & (queue->queue_size - 1u))] =
        (unsigned short)head;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    queue->available_index++;
    available[1] = (unsigned short)queue->available_index;
    return 0;
}

int virtqueue_collect(struct virtqueue *queue, unsigned long long *token,
                      unsigned int *length) {
    if (!token || !length) return -1;
    volatile unsigned short *used =
        (volatile unsigned short *)(queue->base + queue->used_offset);
    unsigned short device_index = used[1];
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    unsigned short pending =
        (unsigned short)(device_index - (unsigned short)queue->used_index);
    if (!pending) return 0;
    if (pending > queue->queue_size) return -1;
    volatile struct vring_used_element *elements =
        (volatile struct vring_used_element *)((volatile unsigned char *)used + 4);
    struct vring_used_element element =
        elements[queue->used_index & (queue->queue_size - 1u)];
    queue->used_index++;
    if (element.id >= queue->queue_size ||
        queue->chain_head[element.id] != element.id ||
        queue->chain_state[element.id] != CHAIN_PUBLISHED)
        return -1;
    unsigned int head = element.id;
    unsigned long long writable_length = 0;
    unsigned int index = head;
    volatile struct vring_desc *desc = descriptors(queue);
    for (unsigned int ordinal = 0; ordinal < queue->chain_length[head];
         ordinal++) {
        if (desc[index].flags & VIRTQUEUE_DESC_WRITE)
            writable_length += desc[index].length;
        index = queue->chain_next[index];
    }
    if (element.length > writable_length) return -1;
    *token = (queue->chain_generation[head] << 16) | head;
    *length = element.length;
    release_chain(queue, head);
    return 1;
}

int virtqueue_kick(struct virtqueue *queue) {
    struct virtio_device *device = queue->device;
    volatile unsigned short *used =
        (volatile unsigned short *)(queue->base + queue->used_offset);
    unsigned short used_flags = used[0];
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (used_flags & VIRTQUEUE_USED_NO_NOTIFY) return 0;
    unsigned long long offset =
        (unsigned long long)queue->notify_offset * device->notify_multiplier;
    if (offset > device->notify.length || 2u > device->notify.length - offset)
        return -1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *(volatile unsigned short *)(device->notify.address + offset) =
        (unsigned short)queue->queue_index;
    return 0;
}
