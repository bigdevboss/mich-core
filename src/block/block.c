#include "block.h"
#include "resource.h"
#include "event.h"
#include "cache.h"

#define BLOCK_REQ_FREE 0
#define BLOCK_REQ_PENDING 1
#define BLOCK_REQ_DONE 2
#define BLOCK_REQ_ERROR 3
#define BLOCK_PAGE_BYTES 4096

struct block_request {
    u32 generation;
    u32 state;
    u32 op;
    u32 lba;
    u32 sectors;
    i32 status;
    u32 transferred;
    u8 data[BLOCK_SECTOR_SIZE];
};

struct block_state {
    struct kernel_object *backing;
    struct kernel_object *event;
    struct kernel_object *transport;
    struct kernel_object *queue;
    block_issue_fn issue;
    block_reap_fn reap;
    u64 tokens[BLOCK_REQUEST_MAX];
    struct block_request requests[BLOCK_REQUEST_MAX];
    u32 sector_count;
    u32 flags;
    u32 generation;
    u32 revoked;
    u32 active;
};

static struct block_state devices[BLOCK_DEVICE_MAX];

static void copy_bytes(u8 *dst, const u8 *src, u32 n) {
    for (u32 i = 0; i < n; i++) dst[i] = src[i];
}

static struct block_state *state_for(const struct kernel_object *object) {
    if (!object || !object->active || object->type != KOBJECT_BLOCK ||
        !object->value || object->value > BLOCK_DEVICE_MAX)
        return 0;
    struct block_state *dev = &devices[object->value - 1];
    return dev->active ? dev : 0;
}

static int transfer(struct block_state *dev, u32 op, u32 lba, u32 sectors,
                    u8 *buffer) {
    struct page_resource *pages = page_resource_get(dev->backing);
    if (!pages || pages->revoked || !buffer) return -1;
    for (u32 n = 0; n < sectors; n++) {
        u32 byte = (lba + n) * BLOCK_SECTOR_SIZE;
        u32 page = byte / BLOCK_PAGE_BYTES;
        u32 off = byte % BLOCK_PAGE_BYTES;
        if (page >= pages->pages || !pages->physical[page]) return -1;
        u8 *sector = (u8 *)(uptr_t)pages->physical[page] + off;
        u8 *part = buffer + n * BLOCK_SECTOR_SIZE;
        if (op == BLOCK_OP_WRITE)
            copy_bytes(sector, part, BLOCK_SECTOR_SIZE);
        else
            copy_bytes(part, sector, BLOCK_SECTOR_SIZE);
    }
    return 0;
}

static int complete_slot(struct block_state *dev, u32 slot) {
    struct block_request *req = &dev->requests[slot];
    if (req->state != BLOCK_REQ_PENDING) return -1;
    u32 bytes = req->sectors * BLOCK_SECTOR_SIZE;
    if (transfer(dev, req->op, req->lba, req->sectors, req->data)) {
        req->state = BLOCK_REQ_ERROR;
        req->status = -1;
        req->transferred = 0;
    } else {
        req->state = BLOCK_REQ_DONE;
        req->status = 0;
        req->transferred = bytes;
    }
    event_signal(dev->event);
    return 0;
}

static int revoke_state(struct block_state *dev) {
    if (!dev || !dev->active || dev->revoked) return -1;
    dev->revoked = 1;
    block_cache_drop_device((u32)(dev - devices) + 1);
    for (u32 slot = 0; slot < BLOCK_REQUEST_MAX; slot++) {
        struct block_request *req = &dev->requests[slot];
        req->generation++;
        if (!req->generation) req->generation = 1;
        req->state = BLOCK_REQ_FREE;
        req->op = 0;
        req->lba = 0;
        req->sectors = 0;
        req->status = 0;
        req->transferred = 0;
        dev->tokens[slot] = 0;
    }
    if (dev->issue) return 0;
    return page_resource_revoke(dev->backing);
}

static void block_destroy(struct kernel_object *object) {
    if (!object || !object->value || object->value > BLOCK_DEVICE_MAX) return;
    struct block_state *dev = &devices[object->value - 1];
    if (!dev->active) return;
    if (!dev->revoked) revoke_state(dev);
    if (dev->event) object_release(dev->event);
    if (dev->queue) object_release(dev->queue);
    if (dev->transport) object_release(dev->transport);
    if (dev->backing) object_release(dev->backing);
    dev->backing = 0;
    dev->event = 0;
    dev->transport = 0;
    dev->queue = 0;
    dev->issue = 0;
    dev->reap = 0;
    dev->sector_count = 0;
    dev->flags = 0;
    dev->generation = 0;
    dev->revoked = 0;
    dev->active = 0;
}

void block_init(void) {
    for (u32 i = 0; i < BLOCK_DEVICE_MAX; i++) {
        devices[i].backing = 0;
        devices[i].event = 0;
        devices[i].transport = 0;
        devices[i].queue = 0;
        devices[i].issue = 0;
        devices[i].reap = 0;
        devices[i].sector_count = 0;
        devices[i].flags = 0;
        devices[i].generation = 0;
        devices[i].revoked = 0;
        devices[i].active = 0;
        for (u32 slot = 0; slot < BLOCK_REQUEST_MAX; slot++) {
            devices[i].requests[slot].generation = 1;
            devices[i].requests[slot].state = BLOCK_REQ_FREE;
            devices[i].requests[slot].op = 0;
            devices[i].requests[slot].lba = 0;
            devices[i].requests[slot].sectors = 0;
            devices[i].requests[slot].status = 0;
            devices[i].requests[slot].transferred = 0;
            devices[i].tokens[slot] = 0;
        }
    }
    block_cache_init();
}

struct kernel_object *block_create(u32 sector_count, u32 flags) {
    if (!sector_count || sector_count > BLOCK_SECTOR_MAX ||
        (flags & ~(BLOCK_FLAG_READ_ONLY | BLOCK_FLAG_DEFER)))
        return 0;
    u32 pages = (sector_count * BLOCK_SECTOR_SIZE + BLOCK_PAGE_BYTES - 1) /
                BLOCK_PAGE_BYTES;
    for (u32 i = 0; i < BLOCK_DEVICE_MAX; i++) {
        struct block_state *dev = &devices[i];
        if (dev->active) continue;
        struct kernel_object *backing = shared_memory_resource_create(pages);
        struct kernel_object *event = backing ?
            event_create(EVENT_AUTO_RESET, 0) : 0;
        struct page_resource *res = backing ? page_resource_get(backing) : 0;
        if (!backing || !event || !res) {
            if (event) object_release(event);
            if (backing) object_release(backing);
            return 0;
        }
        for (u32 page = 0; page < pages; page++) {
            u8 *p = (u8 *)(uptr_t)res->physical[page];
            for (u32 b = 0; b < BLOCK_PAGE_BYTES; b++) p[b] = 0;
        }
        for (u32 slot = 0; slot < BLOCK_REQUEST_MAX; slot++) {
            dev->requests[slot].generation = 1;
            dev->requests[slot].state = BLOCK_REQ_FREE;
            dev->requests[slot].op = 0;
            dev->requests[slot].lba = 0;
            dev->requests[slot].sectors = 0;
            dev->requests[slot].status = 0;
            dev->requests[slot].transferred = 0;
            dev->tokens[slot] = 0;
        }
        dev->backing = backing;
        dev->event = event;
        dev->transport = 0;
        dev->queue = 0;
        dev->issue = 0;
        dev->reap = 0;
        dev->sector_count = sector_count;
        dev->flags = flags;
        dev->generation++;
        if (!dev->generation) dev->generation = 1;
        dev->revoked = 0;
        dev->active = 1;
        struct kernel_object *object = object_create(
            KOBJECT_BLOCK, i + 1, block_destroy);
        if (!object) {
            object_release(event);
            object_release(backing);
            dev->backing = 0;
            dev->event = 0;
            dev->active = 0;
        }
        return object;
    }
    return 0;
}

struct kernel_object *block_bind_transport(u32 sector_count, u32 flags,
    struct kernel_object *transport, struct kernel_object *queue,
    struct kernel_object *dma, block_issue_fn issue, block_reap_fn reap) {
    if (!sector_count || !transport || !queue || !dma || !issue || !reap ||
        (flags & ~(BLOCK_FLAG_READ_ONLY | BLOCK_FLAG_DEFER)))
        return 0;
    flags |= BLOCK_FLAG_DEFER;
    for (u32 i = 0; i < BLOCK_DEVICE_MAX; i++) {
        struct block_state *dev = &devices[i];
        if (dev->active) continue;
        struct kernel_object *event = event_create(EVENT_AUTO_RESET, 0);
        if (!event) return 0;
        if (object_retain(transport)) {
            object_release(event);
            return 0;
        }
        if (object_retain(queue)) {
            object_release(transport);
            object_release(event);
            return 0;
        }
        if (object_retain(dma)) {
            object_release(queue);
            object_release(transport);
            object_release(event);
            return 0;
        }
        for (u32 slot = 0; slot < BLOCK_REQUEST_MAX; slot++) {
            dev->requests[slot].generation = 1;
            dev->requests[slot].state = BLOCK_REQ_FREE;
            dev->requests[slot].op = 0;
            dev->requests[slot].lba = 0;
            dev->requests[slot].sectors = 0;
            dev->requests[slot].status = 0;
            dev->requests[slot].transferred = 0;
            dev->tokens[slot] = 0;
        }
        dev->backing = dma;
        dev->event = event;
        dev->transport = transport;
        dev->queue = queue;
        dev->issue = issue;
        dev->reap = reap;
        dev->sector_count = sector_count;
        dev->flags = flags;
        dev->generation++;
        if (!dev->generation) dev->generation = 1;
        dev->revoked = 0;
        dev->active = 1;
        struct kernel_object *object = object_create(
            KOBJECT_BLOCK, i + 1, block_destroy);
        if (!object) {
            object_release(event);
            object_release(queue);
            object_release(transport);
            object_release(dma);
            dev->backing = 0;
            dev->event = 0;
            dev->transport = 0;
            dev->queue = 0;
            dev->issue = 0;
            dev->reap = 0;
            dev->active = 0;
        }
        return object;
    }
    return 0;
}

int block_info(struct kernel_object *object, struct block_info *info) {
    struct block_state *dev = state_for(object);
    if (!dev || !info || dev->revoked) return -1;
    info->sector_size = BLOCK_SECTOR_SIZE;
    info->sector_count = dev->sector_count;
    info->flags = dev->flags;
    info->generation = dev->generation;
    return 0;
}

int block_submit(struct kernel_object *object, u32 op, u32 lba, u32 sectors,
                 void *buffer, u32 length, u64 *id) {
    struct block_state *dev = state_for(object);
    if (!dev || dev->revoked || !buffer || !id) return -1;
    if ((op != BLOCK_OP_READ && op != BLOCK_OP_WRITE) ||
        !sectors || sectors > BLOCK_IO_SECTORS_MAX ||
        lba >= dev->sector_count ||
        sectors > dev->sector_count - lba ||
        length != sectors * BLOCK_SECTOR_SIZE)
        return -1;
    if (op == BLOCK_OP_WRITE && (dev->flags & BLOCK_FLAG_READ_ONLY))
        return -1;
    for (u32 slot = 0; slot < BLOCK_REQUEST_MAX; slot++) {
        struct block_request *req = &dev->requests[slot];
        if (req->state != BLOCK_REQ_FREE) continue;
        req->op = op;
        req->lba = lba;
        req->sectors = sectors;
        req->status = 0;
        req->transferred = 0;
        if (op == BLOCK_OP_WRITE)
            copy_bytes(req->data, (const u8 *)buffer, length);
        else
            for (u32 i = 0; i < length; i++) req->data[i] = 0;
        req->state = BLOCK_REQ_PENDING;
        *id = ((u64)req->generation << 32) | (slot + 1);
        if (dev->issue) {
            if (dev->issue(dev->queue, dev->backing, slot, op, lba,
                           req->data, &dev->tokens[slot])) {
                req->state = BLOCK_REQ_FREE;
                req->op = 0;
                req->lba = 0;
                req->sectors = 0;
                *id = 0;
                return -1;
            }
            return 0;
        }
        // Default ramdisk completes in submit. DEFER leaves PENDING so a
        // later block_service (or IRQ backend) can finish the slot.
        if (!(dev->flags & BLOCK_FLAG_DEFER))
            complete_slot(dev, slot);
        if (op == BLOCK_OP_READ && req->state == BLOCK_REQ_DONE)
            copy_bytes((u8 *)buffer, req->data, length);
        return 0;
    }
    return -1;
}

int block_service(struct kernel_object *object) {
    struct block_state *dev = state_for(object);
    if (!dev || dev->revoked) return -1;
    u32 n = 0;
    if (dev->reap) {
        for (;;) {
            u32 slot = 0;
            i32 status = -1;
            u8 data[BLOCK_SECTOR_SIZE];
            int rc = dev->reap(dev->queue, dev->backing, dev->tokens,
                               BLOCK_REQUEST_MAX, &slot, &status, data);
            if (rc < 0) return n ? 0 : -1;
            if (!rc) break;
            if (slot >= BLOCK_REQUEST_MAX) continue;
            struct block_request *req = &dev->requests[slot];
            if (req->state != BLOCK_REQ_PENDING) continue;
            if (!status && req->op == BLOCK_OP_READ)
                copy_bytes(req->data, data, BLOCK_SECTOR_SIZE);
            req->state = status ? BLOCK_REQ_ERROR : BLOCK_REQ_DONE;
            req->status = status;
            req->transferred = status ? 0 : req->sectors * BLOCK_SECTOR_SIZE;
            dev->tokens[slot] = 0;
            event_signal(dev->event);
            n++;
        }
        return n ? 0 : -1;
    }
    for (u32 slot = 0; slot < BLOCK_REQUEST_MAX; slot++)
        if (!complete_slot(dev, slot)) n++;
    return n ? 0 : -1;
}

int block_collect(struct kernel_object *object, u64 id,
                  i32 *status, u32 *transferred, void *buffer, u32 length) {
    struct block_state *dev = state_for(object);
    u32 low = (u32)id;
    u32 generation = (u32)(id >> 32);
    if (!dev || !status || !transferred || !low ||
        low > BLOCK_REQUEST_MAX || !generation)
        return -1;
    struct block_request *req = &dev->requests[low - 1];
    if (req->generation != generation ||
        (req->state != BLOCK_REQ_DONE && req->state != BLOCK_REQ_ERROR))
        return -1;
    *status = req->status;
    *transferred = req->transferred;
    if (buffer && req->op == BLOCK_OP_READ && req->state == BLOCK_REQ_DONE) {
        if (length < req->transferred) return -1;
        copy_bytes((u8 *)buffer, req->data, req->transferred);
    }
    req->generation++;
    if (!req->generation) req->generation = 1;
    req->state = BLOCK_REQ_FREE;
    req->op = 0;
    req->lba = 0;
    req->sectors = 0;
    req->status = 0;
    req->transferred = 0;
    return 0;
}

int block_io(struct kernel_object *object, u32 op, u32 lba, u32 sectors,
             void *buffer, u32 length) {
    u64 id = 0;
    i32 status = -1;
    u32 transferred = 0;
    if (block_submit(object, op, lba, sectors, buffer, length, &id))
        return -1;
    if (!block_collect(object, id, &status, &transferred, buffer, length))
        return status;
    // Transport backends complete from the used ring after notify.
    for (u32 spin = 0; spin < 1000000; spin++) {
        block_service(object);
        if (!block_collect(object, id, &status, &transferred, buffer, length))
            return status;
        __asm__ volatile("pause");
    }
    return -1;
}

int block_revoke(struct kernel_object *object) {
    return revoke_state(state_for(object));
}

struct kernel_object *block_wait_event(struct kernel_object *object) {
    struct block_state *dev = state_for(object);
    return dev ? dev->event : 0;
}

u32 block_active_count(void) {
    u32 n = 0;
    for (u32 i = 0; i < BLOCK_DEVICE_MAX; i++)
        if (devices[i].active) n++;
    return n;
}
