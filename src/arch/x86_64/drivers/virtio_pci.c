#include "virtio_pci.h"
#include "pci64.h"
#include "resource.h"
#include "object.h"
#include "vm64.h"
#include "irq.h"
#include "net_buffer.h"
#include "msix64.h"

static struct virtio_device_info devices[VIRTIO_PCI_DEVICE_MAX];
static struct virtqueue_info queues[VIRTIO_QUEUE_MAX];

static u16 read16(volatile u8 *base, u32 offset) {
    return *(volatile u16 *)(base + offset);
}

static u32 read32(volatile u8 *base, u32 offset) {
    return *(volatile u32 *)(base + offset);
}

static void write16(volatile u8 *base, u32 offset, u16 value) {
    *(volatile u16 *)(base + offset) = value;
}

static void write32(volatile u8 *base, u32 offset, u32 value) {
    *(volatile u32 *)(base + offset) = value;
}

static void write64(volatile u8 *base, u32 offset, u64 value) {
    *(volatile u32 *)(base + offset) = (u32)value;
    *(volatile u32 *)(base + offset + 4) = (u32)(value >> 32);
}

static int map_region(struct kernel_object *pci, u8 bar_index,
                      u32 offset, u32 length, struct virtio_pci_region *region) {
    const struct pci_resource *resource = pci_resource_get(pci);
    if (!resource || bar_index >= 6 || !length) return -1;
    const struct pci_bar_resource *bar = &resource->bars[bar_index];
    if (!bar->address || !bar->length || (bar->flags & 1) ||
        offset > bar->length || length > bar->length - offset ||
        bar->address > ~0ULL - offset)
        return -1;
    u64 physical = bar->address + offset;
    paddr_t page = physical & ~0xFFFULL;
    usize_t page_offset = physical & 0xFFF;
    usize_t mapping_length =
        (page_offset + length + 0xFFF) & ~(usize_t)0xFFF;
    void *mapping = vm64_ioremap(page, mapping_length, VM64_CACHE_UC);
    if (!mapping) return -1;
    region->bar = bar_index;
    region->offset = offset;
    region->length = length;
    region->mapping = mapping;
    region->mapping_length = mapping_length;
    region->address = (volatile u8 *)mapping + page_offset;
    return 0;
}

static void unmap_region(struct virtio_pci_region *region) {
    if (region->mapping) vm64_iounmap(region->mapping, region->mapping_length);
    region->bar = 0;
    region->offset = 0;
    region->length = 0;
    region->mapping = 0;
    region->mapping_length = 0;
    region->address = 0;
}

struct virtio_device_info *virtio_pci_get(struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_VIRTIO_DEVICE ||
        !object->value || object->value > VIRTIO_PCI_DEVICE_MAX)
        return 0;
    struct virtio_device_info *device = &devices[object->value - 1];
    return device->active ? device : 0;
}

static void virtio_destroy(struct kernel_object *object) {
    if (!object || !object->value || object->value > VIRTIO_PCI_DEVICE_MAX)
        return;
    struct virtio_device_info *device = &devices[object->value - 1];
    if (!device->active) return;
    if (device->common.address) device->common.address[20] = 0;
    if (device->pci) pci64_quiesce(device->pci);
    unmap_region(&device->device);
    unmap_region(&device->isr);
    unmap_region(&device->notify);
    unmap_region(&device->common);
    if (device->pci) object_release(device->pci);
    device->pci = 0;
    device->notify_multiplier = 0;
    device->device_features = 0;
    device->driver_features = 0;
    device->negotiated = 0;
    device->active = 0;
}

void virtio_pci_init(void) {
    for (u32 index = 0; index < VIRTIO_PCI_DEVICE_MAX; index++) {
        devices[index].pci = 0;
        devices[index].common.mapping = 0;
        devices[index].notify.mapping = 0;
        devices[index].isr.mapping = 0;
        devices[index].device.mapping = 0;
        devices[index].active = 0;
    }
    for (u32 index = 0; index < VIRTIO_QUEUE_MAX; index++) {
        queues[index].device = 0;
        queues[index].dma = 0;
        queues[index].active = 0;
    }
}

struct kernel_object *virtio_pci_create(struct kernel_object *pci) {
    const struct pci_resource *resource = pci_resource_get(pci);
    if (!resource || resource->vendor_id != VIRTIO_PCI_VENDOR ||
        (resource->device_id != VIRTIO_PCI_DEVICE_NET_LEGACY &&
         resource->device_id != VIRTIO_PCI_DEVICE_NET &&
         resource->device_id != VIRTIO_PCI_DEVICE_BLK))
        return 0;
    for (u32 index = 0; index < VIRTIO_PCI_DEVICE_MAX; index++)
        if (devices[index].active && devices[index].pci == pci) return 0;
    u16 status;
    u8 pointer;
    if (pci64_config_read16(pci, 0x06, &status) || !(status & 0x10) ||
        pci64_config_read8(pci, 0x34, &pointer))
        return 0;
    for (u32 slot = 0; slot < VIRTIO_PCI_DEVICE_MAX; slot++) {
        struct virtio_device_info *device = &devices[slot];
        if (device->active) continue;
        u8 seen[32];
        for (u32 index = 0; index < sizeof(seen); index++) seen[index] = 0;
        int failed = 0;
        for (u32 depth = 0; pointer && depth < 48; depth++) {
            u8 id;
            u8 next;
            u8 cap_length;
            if (pointer < 0x40 || pointer > 0xFC || (pointer & 3) ||
                (seen[pointer / 8] & (u8)(1u << (pointer % 8))) ||
                pci64_config_read8(pci, pointer, &id) ||
                pci64_config_read8(pci, pointer + 1, &next)) {
                failed = 1;
                break;
            }
            seen[pointer / 8] |= (u8)(1u << (pointer % 8));
            if (id == 0x09) {
                u8 cfg_type;
                u8 bar;
                u32 offset;
                u32 length;
                if (pointer > 0xEC ||
                    pci64_config_read8(pci, pointer + 2, &cap_length) ||
                    cap_length < 16 || pointer + cap_length > 256 ||
                    pci64_config_read8(pci, pointer + 3, &cfg_type) ||
                    pci64_config_read8(pci, pointer + 4, &bar) ||
                    pci64_config_read32(pci, pointer + 8, &offset) ||
                    pci64_config_read32(pci, pointer + 12, &length)) {
                    failed = 1;
                    break;
                }
                struct virtio_pci_region *region = 0;
                if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) region = &device->common;
                if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) region = &device->notify;
                if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG) region = &device->isr;
                if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) region = &device->device;
                if (region) {
                    if (region->mapping || map_region(pci, bar, offset,
                                                      length, region)) {
                        failed = 1;
                        break;
                    }
                    if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                        if (cap_length < 20 ||
                            pci64_config_read32(pci, pointer + 16,
                                               &device->notify_multiplier)) {
                            failed = 1;
                            break;
                        }
                    }
                }
            }
            if (next & 3) {
                failed = 1;
                break;
            }
            pointer = next;
        }
        if (!failed && (!device->common.address || device->common.length < 56 ||
                        !device->notify.address || device->notify.length < 2 ||
                        !device->isr.address || device->isr.length < 1))
            failed = 1;
        if (failed) {
            unmap_region(&device->device);
            unmap_region(&device->isr);
            unmap_region(&device->notify);
            unmap_region(&device->common);
            device->notify_multiplier = 0;
            return 0;
        }
        if (object_retain(pci)) {
            unmap_region(&device->device);
            unmap_region(&device->isr);
            unmap_region(&device->notify);
            unmap_region(&device->common);
            return 0;
        }
        device->pci = pci;
        device->device_features = 0;
        device->driver_features = 0;
        device->negotiated = 0;
        device->active = 1;
        struct kernel_object *object = object_create(
            KOBJECT_VIRTIO_DEVICE, slot + 1, virtio_destroy);
        if (!object) {
            device->active = 0;
            object_release(pci);
            device->pci = 0;
            unmap_region(&device->device);
            unmap_region(&device->isr);
            unmap_region(&device->notify);
            unmap_region(&device->common);
        }
        return object;
    }
    return 0;
}

int virtio_pci_negotiate(struct kernel_object *object,
                         u64 wanted, u64 required) {
    struct virtio_device_info *device = virtio_pci_get(object);
    if (!device || device->negotiated || (required & ~wanted) ||
        !(wanted & VIRTIO_FEATURE_VERSION_1))
        return -1;
    volatile u8 *common = device->common.address;
    common[20] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    u32 spins = 0;
    while (common[20] && spins++ < 1000000) __asm__ volatile("pause");
    if (common[20]) return -1;
    common[20] = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    write32(common, 0, 0);
    u64 features = read32(common, 4);
    write32(common, 0, 1);
    features |= (u64)read32(common, 4) << 32;
    if (!(features & VIRTIO_FEATURE_VERSION_1) || (required & ~features)) {
        common[20] |= VIRTIO_STATUS_FAILED;
        return -1;
    }
    u64 selected = features & wanted;
    write32(common, 8, 0);
    write32(common, 12, (u32)selected);
    write32(common, 8, 1);
    write32(common, 12, (u32)(selected >> 32));
    common[20] |= VIRTIO_STATUS_FEATURES_OK;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!(common[20] & VIRTIO_STATUS_FEATURES_OK)) {
        common[20] |= VIRTIO_STATUS_FAILED;
        return -1;
    }
    device->device_features = features;
    device->driver_features = selected;
    device->negotiated = 1;
    return 0;
}

int virtio_pci_set_driver_ok(struct kernel_object *object) {
    struct virtio_device_info *device = virtio_pci_get(object);
    if (!device || !device->negotiated ||
        pci64_set_command(device->pci, 6, 0))
        return -1;
    device->common.address[20] |= VIRTIO_STATUS_DRIVER_OK;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (device->common.address[20] & VIRTIO_STATUS_DRIVER_OK) return 0;
    pci64_quiesce(device->pci);
    return -1;
}

int virtio_pci_read_config(struct kernel_object *object, u32 offset,
                           void *buffer, u32 length, u32 *generation) {
    struct virtio_device_info *device = virtio_pci_get(object);
    if (!device || !buffer || !generation || !length ||
        !device->negotiated || !device->device.address ||
        offset > device->device.length ||
        length > device->device.length - offset)
        return -1;
    u8 *output = buffer;
    for (u32 attempt = 0; attempt < 8; attempt++) {
        u8 before = device->common.address[21];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        for (u32 index = 0; index < length; index++)
            output[index] = device->device.address[offset + index];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        u8 after = device->common.address[21];
        if (before == after) {
            *generation = after;
            return 0;
        }
    }
    return -1;
}

struct virtqueue_info *virtqueue_get(struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_VIRTQUEUE ||
        !object->value || object->value > VIRTIO_QUEUE_MAX)
        return 0;
    struct virtqueue_info *queue = &queues[object->value - 1];
    return queue->active ? queue : 0;
}

static void *virtqueue_raw_memory(struct virtqueue_info *queue) {
    const struct dma_resource *memory =
        queue ? dma_resource_get(queue->dma) : 0;
    return memory ? (void *)(uptr_t)memory->physical : 0;
}

static void virtqueue_initialize_state(struct virtqueue_info *queue) {
    u8 *memory = virtqueue_raw_memory(queue);
    if (memory)
        for (u32 index = 0; index < queue->total_bytes; index++)
            memory[index] = 0;
    queue->free_head = 0;
    queue->free_count = queue->queue_size;
    queue->available_index = 0;
    queue->used_index = 0;
    queue->outstanding = 0;
    queue->rejected_used = 0;
    queue->notify_count = 0;
    queue->suppressed_notify_count = 0;
    queue->failed = 0;
    for (u16 index = 0; index < queue->queue_size; index++) {
        queue->free_next[index] = index + 1;
        queue->chain_next[index] = VIRTIO_QUEUE_SIZE_MAX;
        queue->chain_head[index] = VIRTIO_QUEUE_SIZE_MAX;
        queue->chain_length[index] = 0;
        queue->chain_generation[index] = 0;
        queue->chain_state[index] = VIRTQUEUE_CHAIN_FREE;
    }
    if (queue->queue_size)
        queue->free_next[queue->queue_size - 1] = VIRTIO_QUEUE_SIZE_MAX;
}

static void virtqueue_destroy(struct kernel_object *object) {
    if (!object || !object->value || object->value > VIRTIO_QUEUE_MAX) return;
    struct virtqueue_info *queue = &queues[object->value - 1];
    if (!queue->active) return;
    struct virtio_device_info *device = virtio_pci_get(queue->device);
    if (device && device->common.address) {
        device->common.address[20] = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
    if (queue->dma) object_release(queue->dma);
    if (queue->device) object_release(queue->device);
    queue->dma = 0;
    queue->device = 0;
    queue->queue_index = 0;
    queue->queue_size = 0;
    queue->notify_offset = 0;
    queue->free_head = 0;
    queue->free_count = 0;
    queue->available_index = 0;
    queue->used_index = 0;
    queue->generation = 0;
    queue->outstanding = 0;
    queue->descriptor_offset = 0;
    queue->available_offset = 0;
    queue->used_offset = 0;
    queue->total_bytes = 0;
    queue->rejected_used = 0;
    queue->notify_count = 0;
    queue->suppressed_notify_count = 0;
    queue->enabled = 0;
    queue->failed = 0;
    queue->active = 0;
}

struct kernel_object *virtqueue_create(struct kernel_object *device_object,
                                       u16 queue_index, u16 requested_size) {
    struct virtio_device_info *device = virtio_pci_get(device_object);
    if (!device || !device->negotiated) return 0;
    for (u32 index = 0; index < VIRTIO_QUEUE_MAX; index++)
        if (queues[index].active && queues[index].device == device_object &&
            queues[index].queue_index == queue_index)
            return 0;
    volatile u8 *common = device->common.address;
    write16(common, 22, queue_index);
    u16 maximum = read16(common, 24);
    if (!maximum) return 0;
    u16 size = requested_size ? requested_size : maximum;
    if (!size || size > maximum || size > VIRTIO_QUEUE_SIZE_MAX ||
        (size & (size - 1)))
        return 0;
    u32 descriptor_offset = 0;
    u32 available_offset = (u32)size * 16;
    u32 used_offset = (available_offset + 6 + (u32)size * 2 + 3) & ~3u;
    u32 total = used_offset + 6 + (u32)size * 8;
    u32 pages = (total + 4095) / 4096;
    for (u32 index = 0; index < VIRTIO_QUEUE_MAX; index++) {
        struct virtqueue_info *queue = &queues[index];
        if (queue->active) continue;
        struct kernel_object *dma = dma_resource_allocate(pages, 0x3FFFFFFFULL);
        const struct dma_resource *memory = dma_resource_get(dma);
        if (!dma || !memory || object_retain(device_object)) {
            if (dma) object_release(dma);
            return 0;
        }
        u16 notify_offset = read16(common, 30);
        u64 notify_byte = (u64)notify_offset * device->notify_multiplier;
        if (notify_byte > device->notify.length ||
            2 > device->notify.length - notify_byte) {
            object_release(device_object);
            object_release(dma);
            return 0;
        }
        write16(common, 24, size);
        write64(common, 32, memory->physical + descriptor_offset);
        write64(common, 40, memory->physical + available_offset);
        write64(common, 48, memory->physical + used_offset);
        write16(common, 28, 1);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (!read16(common, 28)) {
            object_release(device_object);
            object_release(dma);
            return 0;
        }
        queue->device = device_object;
        queue->dma = dma;
        queue->queue_index = queue_index;
        queue->queue_size = size;
        queue->notify_offset = notify_offset;
        queue->descriptor_offset = descriptor_offset;
        queue->available_offset = available_offset;
        queue->used_offset = used_offset;
        queue->total_bytes = total;
        queue->generation = 0;
        queue->enabled = 1;
        virtqueue_initialize_state(queue);
        queue->active = 1;
        struct kernel_object *object = object_create(
            KOBJECT_VIRTQUEUE, index + 1, virtqueue_destroy);
        if (!object) {
            queue->active = 0;
            common[20] = 0;
            object_release(device_object);
            object_release(dma);
        }
        return object;
    }
    return 0;
}

static int virtqueue_token_head(struct virtqueue_info *queue, u64 token,
                                u16 required_state, u16 *head) {
    u16 index = (u16)token;
    u64 generation = token >> 16;
    if (!token || index >= queue->queue_size || !generation ||
        queue->chain_head[index] != index ||
        queue->chain_generation[index] != generation ||
        queue->chain_state[index] != required_state)
        return -1;
    *head = index;
    return 0;
}

static void virtqueue_release_locked(struct virtqueue_info *queue, u16 head) {
    u16 count = queue->chain_length[head];
    u16 index = head;
    for (u16 ordinal = 0; ordinal < count; ordinal++) {
        u16 next = queue->chain_next[index];
        queue->chain_next[index] = VIRTIO_QUEUE_SIZE_MAX;
        queue->chain_head[index] = VIRTIO_QUEUE_SIZE_MAX;
        queue->chain_length[index] = 0;
        queue->chain_generation[index] = 0;
        queue->chain_state[index] = VIRTQUEUE_CHAIN_FREE;
        queue->free_next[index] = queue->free_head;
        queue->free_head = index;
        queue->free_count++;
        index = next;
    }
}

int virtqueue_chain_allocate(struct kernel_object *object, u16 count,
                             u64 *token) {
    struct virtqueue_info *queue = virtqueue_get(object);
    if (!queue || !token || !count || count > queue->queue_size ||
        queue->failed)
        return -1;
    irq_state_t state = irq_save();
    if (count > queue->free_count) {
        irq_restore(state);
        return -1;
    }
    queue->generation++;
    if (!queue->generation || queue->generation > 0xFFFFFFFFFFFFULL)
        queue->generation = 1;
    u64 generation = queue->generation;
    u16 head = queue->free_head;
    u16 index = head;
    struct virtqueue_descriptor *descriptors =
        (struct virtqueue_descriptor *)((u8 *)virtqueue_raw_memory(queue) +
                                        queue->descriptor_offset);
    for (u16 ordinal = 0; ordinal < count; ordinal++) {
        u16 next = queue->free_next[index];
        queue->free_head = next;
        queue->free_count--;
        queue->chain_head[index] = head;
        queue->chain_generation[index] = generation;
        queue->chain_state[index] = VIRTQUEUE_CHAIN_ALLOCATED;
        queue->chain_next[index] = ordinal + 1 < count ? next :
                                   VIRTIO_QUEUE_SIZE_MAX;
        descriptors[index].address = 0;
        descriptors[index].length = 0;
        descriptors[index].flags = ordinal + 1 < count ?
                                   VIRTQUEUE_DESC_NEXT : 0;
        descriptors[index].next = ordinal + 1 < count ? next : 0;
        index = next;
    }
    queue->chain_length[head] = count;
    *token = (generation << 16) | head;
    irq_restore(state);
    return 0;
}

int virtqueue_descriptor_set(struct kernel_object *object, u64 token,
                             u16 ordinal, u64 address, u32 length,
                             int writable) {
    struct virtqueue_info *queue = virtqueue_get(object);
    if (!queue || !length || address > ~0ULL - (length - 1) || queue->failed)
        return -1;
    irq_state_t state = irq_save();
    u16 head;
    if (virtqueue_token_head(queue, token, VIRTQUEUE_CHAIN_ALLOCATED, &head) ||
        ordinal >= queue->chain_length[head]) {
        irq_restore(state);
        return -1;
    }
    u16 index = head;
    for (u16 current = 0; current < ordinal; current++)
        index = queue->chain_next[index];
    struct virtqueue_descriptor *descriptors =
        (struct virtqueue_descriptor *)((u8 *)virtqueue_raw_memory(queue) +
                                        queue->descriptor_offset);
    u16 flags = descriptors[index].flags & VIRTQUEUE_DESC_NEXT;
    if (writable) flags |= VIRTQUEUE_DESC_WRITE;
    descriptors[index].address = address;
    descriptors[index].length = length;
    descriptors[index].flags = flags;
    irq_restore(state);
    return 0;
}

int virtqueue_descriptor_set_packet(struct kernel_object *object, u64 token,
                                    u16 ordinal, struct kernel_object *pool,
                                    u64 buffer_id, u32 offset, u32 length,
                                    int writable) {
    if (!pool || offset >= NET_PACKET_DATA_MAX || !length ||
        length > NET_PACKET_DATA_MAX - offset)
        return -1;
    u32 state = writable ? NET_BUFFER_DRIVER_RX : NET_BUFFER_DRIVER_TX;
    void *data = packet_pool_data(pool, buffer_id, state);
    if (!data) return -1;
    return virtqueue_descriptor_set(
        object, token, ordinal, (u64)(uptr_t)data + offset, length, writable);
}

int virtqueue_chain_release(struct kernel_object *object, u64 token) {
    struct virtqueue_info *queue = virtqueue_get(object);
    if (!queue) return -1;
    irq_state_t state = irq_save();
    u16 head;
    if (virtqueue_token_head(queue, token, VIRTQUEUE_CHAIN_ALLOCATED, &head)) {
        irq_restore(state);
        return -1;
    }
    virtqueue_release_locked(queue, head);
    irq_restore(state);
    return 0;
}

int virtqueue_publish(struct kernel_object *object, u64 token) {
    struct virtqueue_info *queue = virtqueue_get(object);
    if (!queue || queue->failed || !queue->enabled) return -1;
    irq_state_t state = irq_save();
    u16 head;
    if (virtqueue_token_head(queue, token, VIRTQUEUE_CHAIN_ALLOCATED, &head)) {
        irq_restore(state);
        return -1;
    }
    u16 index = head;
    struct virtqueue_descriptor *descriptors =
        (struct virtqueue_descriptor *)((u8 *)virtqueue_raw_memory(queue) +
                                        queue->descriptor_offset);
    for (u16 ordinal = 0; ordinal < queue->chain_length[head]; ordinal++) {
        if (!descriptors[index].length) {
            irq_restore(state);
            return -1;
        }
        index = queue->chain_next[index];
    }
    index = head;
    for (u16 ordinal = 0; ordinal < queue->chain_length[head]; ordinal++) {
        queue->chain_state[index] = VIRTQUEUE_CHAIN_PUBLISHED;
        index = queue->chain_next[index];
    }
    volatile u16 *available =
        (volatile u16 *)((u8 *)virtqueue_raw_memory(queue) +
                         queue->available_offset);
    available[2 + (queue->available_index & (queue->queue_size - 1))] = head;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    queue->available_index++;
    available[1] = queue->available_index;
    queue->outstanding++;
    irq_restore(state);
    return 0;
}

int virtqueue_collect(struct kernel_object *object,
                      struct virtqueue_completion *completion) {
    struct virtqueue_info *queue = virtqueue_get(object);
    if (!queue || !completion || queue->failed) return -1;
    irq_state_t state = irq_save();
    volatile u16 *used =
        (volatile u16 *)((u8 *)virtqueue_raw_memory(queue) +
                         queue->used_offset);
    u16 device_index = used[1];
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    u16 pending = (u16)(device_index - queue->used_index);
    if (!pending) {
        irq_restore(state);
        return 0;
    }
    if (pending > queue->queue_size) {
        queue->failed = 1;
        irq_restore(state);
        return -1;
    }
    volatile struct virtqueue_used_element *elements =
        (volatile struct virtqueue_used_element *)((u8 *)used + 4);
    struct virtqueue_used_element element =
        elements[queue->used_index & (queue->queue_size - 1)];
    queue->used_index++;
    if (element.id >= queue->queue_size ||
        queue->chain_head[element.id] != element.id ||
        queue->chain_state[element.id] != VIRTQUEUE_CHAIN_PUBLISHED) {
        queue->rejected_used++;
        irq_restore(state);
        return -1;
    }
    u16 head = (u16)element.id;
    u64 writable_length = 0;
    u16 index = head;
    struct virtqueue_descriptor *descriptors =
        (struct virtqueue_descriptor *)((u8 *)virtqueue_raw_memory(queue) +
                                        queue->descriptor_offset);
    for (u16 ordinal = 0; ordinal < queue->chain_length[head]; ordinal++) {
        if (descriptors[index].flags & VIRTQUEUE_DESC_WRITE)
            writable_length += descriptors[index].length;
        index = queue->chain_next[index];
    }
    if (element.length > writable_length) {
        queue->failed = 1;
        irq_restore(state);
        return -1;
    }
    completion->token = (queue->chain_generation[head] << 16) | head;
    completion->length = element.length;
    virtqueue_release_locked(queue, head);
    queue->outstanding--;
    irq_restore(state);
    return 1;
}

int virtqueue_collect_batch(struct kernel_object *object,
                            struct virtqueue_completion *completions,
                            u32 maximum) {
    if (!object || !completions || !maximum ||
        maximum > VIRTIO_QUEUE_SIZE_MAX)
        return -1;
    u32 count = 0;
    while (count < maximum) {
        int result = virtqueue_collect(object, &completions[count]);
        if (result < 0) return count ? (int)count : -1;
        if (!result) break;
        count++;
    }
    return (int)count;
}

int virtqueue_reset_state(struct kernel_object *object) {
    struct virtqueue_info *queue = virtqueue_get(object);
    struct virtio_device_info *device = queue ? virtio_pci_get(queue->device) : 0;
    if (!queue || !device || (device->common.address[20] &
                              VIRTIO_STATUS_DRIVER_OK))
        return -1;
    irq_state_t state = irq_save();
    virtqueue_initialize_state(queue);
    irq_restore(state);
    return 0;
}

void *virtqueue_memory(struct kernel_object *object) {
    struct virtqueue_info *queue = virtqueue_get(object);
    return virtqueue_raw_memory(queue);
}

struct kernel_object *virtqueue_dma_object(struct kernel_object *object) {
    struct virtqueue_info *queue = virtqueue_get(object);
    return queue ? queue->dma : 0;
}

int virtqueue_set_msix(struct kernel_object *object,
                       struct kernel_object *irq) {
    struct virtqueue_info *queue = virtqueue_get(object);
    struct virtio_device_info *device = queue ?
        virtio_pci_get(queue->device) : 0;
    u32 entry;
    if (!queue || !device || !irq ||
        msix64_irq_entry(irq, device->pci, &entry) || entry > 0xFFFFu)
        return -1;
    volatile u8 *common = device->common.address;
    write16(common, 22, queue->queue_index);
    write16(common, 26, (u16)entry);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return read16(common, 26) == (u16)entry ? 0 : -1;
}

int virtqueue_notify(struct kernel_object *object) {
    struct virtqueue_info *queue = virtqueue_get(object);
    struct virtio_device_info *device = queue ? virtio_pci_get(queue->device) : 0;
    if (!queue || !device || !queue->enabled || queue->failed) return -1;
    volatile u16 *used =
        (volatile u16 *)((u8 *)virtqueue_raw_memory(queue) +
                         queue->used_offset);
    u16 used_flags = used[0];
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (used_flags & VIRTQUEUE_USED_NO_NOTIFY) {
        queue->suppressed_notify_count++;
        return 0;
    }
    u64 offset = (u64)queue->notify_offset * device->notify_multiplier;
    if (offset > device->notify.length || 2 > device->notify.length - offset)
        return -1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *(volatile u16 *)(device->notify.address + offset) = queue->queue_index;
    queue->notify_count++;
    return 0;
}

u32 virtio_pci_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < VIRTIO_PCI_DEVICE_MAX; index++)
        if (devices[index].active) count++;
    return count;
}

u32 virtqueue_active_count(void) {
    u32 count = 0;
    for (u32 index = 0; index < VIRTIO_QUEUE_MAX; index++)
        if (queues[index].active) count++;
    return count;
}
