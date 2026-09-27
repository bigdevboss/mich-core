#include <mich/syscall.h>
#include <mich/driver.h>
#include <mich/virtio.h>
#include <mich/ring.h>
#include <mich/memory.h>
#include <mich/event.h>
#include <mich/block.h>

#include "capsule.h"

// Userspace virtio-blk capsule: brings the device up, registers a shared-memory
// transport with the kernel block layer, and serves requests off the request
// ring by driving the virtqueue. Data moves without a copy: the kernel fills the
// shared pool, the device DMAs that pool in place, and the kernel drains it. The
// capsule is the only virtio-blk driver; the kernel keeps no in-kernel one.

static int transition(struct virtio_blk_capsule *capsule,
                      unsigned int expected, unsigned int next) {
    if (!capsule || capsule->state != expected || next != expected + 1)
        return -1;
    capsule->state = next;
    return 0;
}

static void write_le32(volatile unsigned char *data, unsigned int value) {
    data[0] = (unsigned char)value;
    data[1] = (unsigned char)(value >> 8);
    data[2] = (unsigned char)(value >> 16);
    data[3] = (unsigned char)(value >> 24);
}

static void write_le64(volatile unsigned char *data, unsigned long long value) {
    write_le32(data, (unsigned int)value);
    write_le32(data + 4, (unsigned int)(value >> 32));
}

static int bootstrap(struct virtio_blk_capsule *capsule) {
    struct mich_driver_bootstrap_info info;
    if (mich_driver_bootstrap(&info) ||
        info.abi_version != MICH_DRIVER_ABI_VERSION ||
        info.size != sizeof(info) || info.vendor_id != 0x1AF4 ||
        (info.device_id != VIRTIO_BLK_DEVICE_MODERN &&
         info.device_id != VIRTIO_BLK_DEVICE_TRANSITIONAL))
        return -1;
    for (unsigned int index = 0; index < info.resource_count; index++) {
        struct mich_driver_resource_info *resource = &info.resources[index];
        if (resource->kind == MICH_DRIVER_RESOURCE_PCI && !resource->index)
            capsule->pci_handle = resource->handle;
        if (resource->kind == MICH_DRIVER_RESOURCE_MSIX_IRQ &&
            resource->index == 0)
            capsule->config_irq_handle = resource->handle;
        if (resource->kind == MICH_DRIVER_RESOURCE_MSIX_IRQ &&
            resource->index == 1)
            capsule->queue_irq_handle = resource->handle;
        if (resource->kind == MICH_DRIVER_RESOURCE_BRIDGE)
            capsule->bridge_handle = resource->handle;
    }
    // Require the manifest's grants up front so an incomplete grant fails at
    // bring-up, not at first I/O.
    if (!capsule->pci_handle || !capsule->bridge_handle ||
        !capsule->queue_irq_handle)
        return -1;
    capsule->restart_count = info.restart_count;
    capsule->device_handle = mich_virtio_open(capsule->pci_handle);
    if ((int)capsule->device_handle <= 0) return -1;
    return transition(capsule, VIRTIO_BLK_STATE_CREATED,
                      VIRTIO_BLK_STATE_BOOTSTRAPPED);
}

static int negotiate(struct virtio_blk_capsule *capsule) {
    struct mich_virtio_feature_request request;
    request.wanted = MICH_VIRTIO_FEATURE_VERSION_1 |
                     VIRTIO_BLK_FEATURE_RO |
                     VIRTIO_BLK_FEATURE_BLK_SIZE;
    request.required = MICH_VIRTIO_FEATURE_VERSION_1;
    request.device_features = 0;
    request.driver_features = 0;
    if (mich_virtio_negotiate(capsule->device_handle, &request) ||
        (request.driver_features & request.required) != request.required)
        return -1;
    capsule->negotiated_features = request.driver_features;
    capsule->read_only =
        (request.driver_features & VIRTIO_BLK_FEATURE_RO) ? 1u : 0u;
    return transition(capsule, VIRTIO_BLK_STATE_BOOTSTRAPPED,
                      VIRTIO_BLK_STATE_FEATURES);
}

static int read_config(struct virtio_blk_capsule *capsule) {
    struct mich_virtio_config_request request;
    request.offset = 0;
    // Capacity is the first 8 bytes; the logical block size sits at offset 20
    // and is only present when BLK_SIZE was negotiated (virtio spec 5.2.4).
    request.length =
        (capsule->negotiated_features & VIRTIO_BLK_FEATURE_BLK_SIZE) ? 24 : 8;
    request.generation = 0;
    request.reserved = 0;
    for (unsigned int index = 0; index < MICH_VIRTIO_CONFIG_DATA_MAX; index++)
        request.data[index] = 0;
    if (mich_virtio_read_config(capsule->device_handle, &request)) return -1;
    unsigned long long capacity = 0;
    for (unsigned int index = 0; index < 8; index++)
        capacity |= (unsigned long long)request.data[index] << (index * 8);
    if (!capacity) return -1;
    capsule->capacity_sectors = capacity;
    if (capsule->negotiated_features & VIRTIO_BLK_FEATURE_BLK_SIZE) {
        unsigned int block_size = (unsigned int)request.data[20] |
                                  ((unsigned int)request.data[21] << 8) |
                                  ((unsigned int)request.data[22] << 16) |
                                  ((unsigned int)request.data[23] << 24);
        // The block layer and ABI are fixed at 512-byte sectors.
        if (block_size != MICH_BLOCK_SECTOR_SIZE) return -1;
        capsule->block_size = block_size;
    } else {
        capsule->block_size = MICH_BLOCK_SECTOR_SIZE;
    }
    return transition(capsule, VIRTIO_BLK_STATE_FEATURES,
                      VIRTIO_BLK_STATE_CONFIG);
}

static int setup_transport(struct virtio_blk_capsule *capsule) {
    int pool = mich_shared_memory_create(VIRTIO_BLK_POOL_PAGES);
    int req_ring = mich_ring_create(VIRTIO_BLK_RING_CAPACITY,
                                    sizeof(struct mich_block_driver_request));
    int cmp_ring = mich_ring_create(VIRTIO_BLK_RING_CAPACITY,
                                    sizeof(struct mich_block_driver_completion));
    int scratch = mich_shared_memory_create(1);
    if (pool <= 0 || req_ring <= 0 || cmp_ring <= 0 || scratch <= 0 ||
        mich_ring_map((unsigned int)req_ring, VIRTIO_BLK_REQ_RING_ADDRESS) ||
        mich_ring_map((unsigned int)cmp_ring, VIRTIO_BLK_CMP_RING_ADDRESS) ||
        mich_page_map((unsigned int)scratch, VIRTIO_BLK_SCRATCH_ADDRESS))
        return -1;
    capsule->pool_handle = (unsigned int)pool;
    capsule->request_ring_handle = (unsigned int)req_ring;
    capsule->completion_ring_handle = (unsigned int)cmp_ring;
    capsule->scratch_handle = (unsigned int)scratch;

    struct mich_block_driver_register_request request;
    unsigned char *bytes = (unsigned char *)&request;
    for (unsigned int index = 0; index < sizeof(request); index++)
        bytes[index] = 0;
    request.request_ring_handle = capsule->request_ring_handle;
    request.completion_ring_handle = capsule->completion_ring_handle;
    request.pool_handle = capsule->pool_handle;
    request.sector_size = MICH_BLOCK_SECTOR_SIZE;
    request.sector_count = (unsigned int)capsule->capacity_sectors;
    request.flags = capsule->read_only ? MICH_BLOCK_FLAG_READ_ONLY : 0u;
    request.name[0] = 'v';
    request.name[1] = 'b';
    request.name[2] = 'l';
    request.name[3] = 'k';
    if (mich_block_interface_create(&request) || !request.device_handle)
        return -1;
    capsule->block_device_handle = request.device_handle;
    return 0;
}

static int start_device(struct virtio_blk_capsule *capsule) {
    struct mich_virtqueue_create_request request;
    request.device_handle = capsule->device_handle;
    request.queue_index = 0;
    request.queue_size = 128;
    request.queue_handle = 0;
    request.descriptor_offset = 0;
    request.available_offset = 0;
    request.used_offset = 0;
    request.total_bytes = 0;
    if (mich_virtqueue_create(&request) || !request.queue_handle ||
        mich_virtqueue_map(request.queue_handle, VIRTIO_BLK_QUEUE_ADDRESS))
        return -1;
    capsule->queue_handle = request.queue_handle;
    capsule->queue_size = request.queue_size;
    // Completions are polled in drive_device, so the queue MSI-X vector is left
    // unconfigured: the device posts to the used ring either way.
    return mich_virtio_driver_ok(capsule->device_handle);
}

static volatile unsigned char *ring_descriptor(unsigned long long base,
                                               unsigned long long index) {
    volatile struct mich_ring_shared_header *header =
        (volatile struct mich_ring_shared_header *)base;
    return (volatile unsigned char *)base + header->data_offset +
           (index % header->capacity) * header->descriptor_size;
}

// Build the three-descriptor virtio-blk chain (header, data, status), submit it
// and poll for completion. The header and status live in the scratch pool; the
// data descriptor points straight at the shared pool slot the kernel filled, so
// nothing is copied on this path.
static int drive_device(struct virtio_blk_capsule *capsule, unsigned int op,
                        unsigned int lba, unsigned int sectors,
                        unsigned int pool_offset, unsigned int slot) {
    if (slot >= VIRTIO_BLK_SLOT_MAX || sectors != 1 ||
        (op != MICH_BLOCK_OP_READ && op != MICH_BLOCK_OP_WRITE))
        return -1;
    unsigned int read = (op == MICH_BLOCK_OP_READ) ? 1u : 0u;
    unsigned int header_off = slot * VIRTIO_BLK_SCRATCH_STRIDE;
    unsigned int status_off = header_off + VIRTIO_BLK_HEADER_SIZE;
    volatile unsigned char *scratch =
        (volatile unsigned char *)VIRTIO_BLK_SCRATCH_ADDRESS;
    write_le32(scratch + header_off, read ? VIRTIO_BLK_T_IN : VIRTIO_BLK_T_OUT);
    write_le32(scratch + header_off + 4, 0);
    write_le64(scratch + header_off + 8, lba);
    // Poison the status so a device that never wrote it is not read as success.
    scratch[status_off] = 0xFF;

    struct mich_virtqueue_chain_request chain;
    chain.queue_handle = capsule->queue_handle;
    chain.descriptor_count = 3;
    chain.reserved = 0;
    chain.token = 0;
    if (mich_virtqueue_chain_allocate(&chain)) return -1;

    struct mich_virtqueue_region_request region;
    region.queue_handle = capsule->queue_handle;
    region.token = chain.token;
    region.pool_handle = capsule->scratch_handle;
    region.offset = header_off;
    region.length = VIRTIO_BLK_HEADER_SIZE;
    region.ordinal = 0;
    region.writable = 0;
    int failed = mich_virtqueue_set_region(&region);
    region.pool_handle = capsule->pool_handle;
    region.offset = pool_offset;
    region.length = sectors * MICH_BLOCK_SECTOR_SIZE;
    region.ordinal = 1;
    region.writable = (unsigned short)read;
    failed |= mich_virtqueue_set_region(&region);
    region.pool_handle = capsule->scratch_handle;
    region.offset = status_off;
    region.length = 1;
    region.ordinal = 2;
    region.writable = 1;
    failed |= mich_virtqueue_set_region(&region);
    if (failed || mich_virtqueue_publish(&chain) ||
        mich_virtqueue_notify(capsule->queue_handle)) {
        mich_virtqueue_chain_release(&chain);
        return -1;
    }

    struct mich_virtqueue_completion_result result;
    result.queue_handle = capsule->queue_handle;
    for (unsigned int attempt = 0; attempt < 2000000u; attempt++) {
        result.length = 0;
        result.token = 0;
        int collected = mich_virtqueue_collect(&result);
        if (collected < 0) return -1;
        if (collected == 1 && result.token == chain.token)
            return scratch[status_off] == VIRTIO_BLK_S_OK ? 0 : -1;
        if (!collected) mich_yield();
    }
    return -1;
}

// Consume one request the kernel produced, drive it, and publish the completion.
// Returns 1 when a request was handled, 0 when the ring is empty, negative on a
// transport fault (the supervisor then restarts the capsule).
static int serve_request(struct virtio_blk_capsule *capsule) {
    volatile struct mich_ring_shared_header *req =
        (volatile struct mich_ring_shared_header *)VIRTIO_BLK_REQ_RING_ADDRESS;
    if (req->producer == req->consumer) return 0;
    volatile struct mich_block_driver_request *entry =
        (volatile struct mich_block_driver_request *)ring_descriptor(
            VIRTIO_BLK_REQ_RING_ADDRESS, req->consumer);
    unsigned long long request_id = entry->request_id;
    unsigned int op = entry->op;
    unsigned int lba = entry->lba;
    unsigned int sectors = entry->sectors;
    unsigned int pool_offset = entry->pool_offset;
    if (mich_ring_consume(capsule->request_ring_handle, 1)) return -1;

    unsigned int slot = pool_offset / MICH_BLOCK_SECTOR_SIZE;
    int status = drive_device(capsule, op, lba, sectors, pool_offset, slot);

    volatile struct mich_ring_shared_header *cmp =
        (volatile struct mich_ring_shared_header *)VIRTIO_BLK_CMP_RING_ADDRESS;
    volatile struct mich_block_driver_completion *done =
        (volatile struct mich_block_driver_completion *)ring_descriptor(
            VIRTIO_BLK_CMP_RING_ADDRESS, cmp->producer);
    done->request_id = request_id;
    done->status = status;
    done->transferred = status ? 0u : sectors * MICH_BLOCK_SECTOR_SIZE;
    if (mich_ring_submit(capsule->completion_ring_handle, 1)) return -1;
    return 1;
}

// Exercise the whole path end to end: as its own client the capsule submits a
// write and a read-back through the kernel block layer, serving each in between,
// and checks the bytes survive the round trip. This proves the ring transport,
// the region descriptors and the data-pool zero-copy before real clients arrive.
static int self_test(struct virtio_blk_capsule *capsule) {
    if (capsule->read_only || capsule->capacity_sectors <= 5) return 0;
    struct mich_block_io_request io;
    for (unsigned int index = 0; index < MICH_BLOCK_SECTOR_SIZE; index++)
        io.data[index] = (unsigned char)((index * 7u + 19u) & 0xFFu);
    io.device_handle = capsule->block_device_handle;
    io.op = MICH_BLOCK_OP_WRITE;
    io.lba = 5;
    io.sectors = 1;
    io.request_id = 0;
    io.status = 0;
    io.transferred = 0;
    if (mich_block_submit(&io)) return -1;
    unsigned long long write_id = io.request_id;
    if (serve_request(capsule) != 1 ||
        mich_block_service(capsule->block_device_handle)) return -1;
    io.request_id = write_id;
    if (mich_block_collect(&io) || io.status) return -1;

    for (unsigned int index = 0; index < MICH_BLOCK_SECTOR_SIZE; index++)
        io.data[index] = 0;
    io.device_handle = capsule->block_device_handle;
    io.op = MICH_BLOCK_OP_READ;
    io.lba = 5;
    io.sectors = 1;
    io.request_id = 0;
    if (mich_block_submit(&io)) return -1;
    unsigned long long read_id = io.request_id;
    if (serve_request(capsule) != 1 ||
        mich_block_service(capsule->block_device_handle)) return -1;
    io.request_id = read_id;
    if (mich_block_collect(&io) || io.status ||
        io.transferred != MICH_BLOCK_SECTOR_SIZE)
        return -1;
    for (unsigned int index = 0; index < MICH_BLOCK_SECTOR_SIZE; index++)
        if (io.data[index] != (unsigned char)((index * 7u + 19u) & 0xFFu))
            return -1;
    return 0;
}

// Steady state: drain every request the kernel has queued, then park. No
// kernel-to-capsule submit doorbell exists yet, so the capsule blocks on an
// event nothing signals and idles at zero CPU while it stays registered as the
// live virtio-blk driver. A wakeup doorbell is future work.
static void serve_loop(struct virtio_blk_capsule *capsule) {
    int idle = mich_event_create(MICH_EVENT_MANUAL_RESET, 0);
    if (idle <= 0) return;
    for (;;) {
        int served;
        do {
            served = serve_request(capsule);
            if (served < 0) return;
        } while (served);
        if (mich_event_wait((unsigned int)idle)) return;
    }
}

int main(unsigned long long argument) {
    struct virtio_blk_capsule capsule;
    unsigned char *bytes = (unsigned char *)&capsule;
    for (unsigned int index = 0; index < sizeof(capsule); index++)
        bytes[index] = 0;
    if ((unsigned int)argument != VIRTIO_BLK_MAGIC || bootstrap(&capsule))
        return 1;
    if (negotiate(&capsule)) return 2;
    if (read_config(&capsule)) return 3;
    mich_write("Mich virtio-blk: bootstrap pass\n");
    if (setup_transport(&capsule)) return 4;
    if (start_device(&capsule)) return 5;
    if (self_test(&capsule)) return 6;
    mich_write("Mich virtio-blk: serve pass\n");
    serve_loop(&capsule);
    return 7;
}
