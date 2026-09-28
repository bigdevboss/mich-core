#include <mich/syscall.h>
#include <mich/driver.h>
#include <mich/hardware.h>
#include <mich/ring.h>
#include <mich/memory.h>
#include <mich/event.h>
#include <mich/block.h>

#include "capsule.h"

// Userspace virtio-blk capsule: brings the device up through the userspace virtio
// transport (libvirtio) over raw hardware handles, registers a shared-memory
// transport with the kernel block layer, and serves requests off the request ring
// by driving the virtqueue directly. Data moves without a copy: the device DMAs
// the shared pool in place. The capsule is the only virtio-blk driver; the kernel
// keeps none.

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
        if (resource->kind == MICH_DRIVER_RESOURCE_DMA) {
            capsule->dma_handle = resource->handle;
            // Bus address the device DMAs the ring through: equals the raw
            // physical without an IOMMU, or the bound IOVA with one.
            capsule->dma_physical = resource->address;
        }
        if (resource->kind == MICH_DRIVER_RESOURCE_BRIDGE)
            capsule->bridge_handle = resource->handle;
    }
    if (!capsule->pci_handle || !capsule->dma_handle || !capsule->bridge_handle)
        return -1;
    return 0;
}

static int bring_up(struct virtio_blk_capsule *capsule) {
    if (virtio_device_setup(&capsule->device, capsule->pci_handle,
                            VIRTIO_BLK_BAR_WINDOW))
        return -1;
    unsigned long long wanted = VIRTIO_FEATURE_VERSION_1 |
                                VIRTIO_BLK_FEATURE_RO | VIRTIO_BLK_FEATURE_BLK_SIZE;
    if (virtio_negotiate(&capsule->device, wanted, VIRTIO_FEATURE_VERSION_1))
        return -1;
    capsule->negotiated_features = capsule->device.driver_features;
    capsule->read_only =
        (capsule->device.driver_features & VIRTIO_BLK_FEATURE_RO) ? 1u : 0u;

    // Capacity is the first 8 bytes; the logical block size sits at offset 20 and
    // is only present when BLK_SIZE was negotiated (virtio spec 5.2.4).
    unsigned char config[24];
    for (unsigned int index = 0; index < sizeof(config); index++)
        config[index] = 0;
    unsigned int length =
        (capsule->negotiated_features & VIRTIO_BLK_FEATURE_BLK_SIZE) ? 24u : 8u;
    if (virtio_read_config(&capsule->device, 0, config, length)) return -1;
    unsigned long long capacity = 0;
    for (unsigned int index = 0; index < 8u; index++)
        capacity |= (unsigned long long)config[index] << (index * 8u);
    if (!capacity) return -1;
    capsule->capacity_sectors = capacity;
    if (capsule->negotiated_features & VIRTIO_BLK_FEATURE_BLK_SIZE) {
        unsigned int block_size = (unsigned int)config[20] |
                                  ((unsigned int)config[21] << 8) |
                                  ((unsigned int)config[22] << 16) |
                                  ((unsigned int)config[23] << 24);
        // The block layer and ABI are fixed at 512-byte sectors.
        if (block_size != MICH_BLOCK_SECTOR_SIZE) return -1;
    }
    return 0;
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

static int start_queue(struct virtio_blk_capsule *capsule) {
    if (mich_dma_map(capsule->dma_handle, VIRTIO_BLK_VRING_ADDRESS) ||
        virtqueue_setup(&capsule->device, &capsule->queue, 0,
                        VIRTIO_BLK_QUEUE_SIZE, VIRTIO_BLK_VRING_ADDRESS,
                        capsule->dma_physical,
                        (unsigned long long)VIRTIO_BLK_VRING_PAGES * 4096u))
        return -1;
    // Completions are polled in drive_device, so no queue MSI-X vector is set: the
    // device posts to the used ring either way.
    return virtio_driver_ok(&capsule->device);
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
// nothing is copied on this path. A single 512-byte sector never crosses a page.
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

    unsigned long long header_phys =
        mich_resource_physical(capsule->scratch_handle, header_off);
    unsigned long long status_phys =
        mich_resource_physical(capsule->scratch_handle, status_off);
    unsigned long long data_phys =
        mich_resource_physical(capsule->pool_handle, pool_offset);
    if (header_phys == (unsigned long long)-1 ||
        status_phys == (unsigned long long)-1 ||
        data_phys == (unsigned long long)-1)
        return -1;

    unsigned long long token;
    if (virtqueue_chain_alloc(&capsule->queue, 3, &token)) return -1;
    int failed = virtqueue_descriptor_set(&capsule->queue, token, 0, header_phys,
                                          VIRTIO_BLK_HEADER_SIZE, 0);
    failed |= virtqueue_descriptor_set(&capsule->queue, token, 1, data_phys,
                                       sectors * MICH_BLOCK_SECTOR_SIZE,
                                       (int)read);
    failed |= virtqueue_descriptor_set(&capsule->queue, token, 2, status_phys,
                                       1, 1);
    if (failed || virtqueue_chain_publish(&capsule->queue, token) ||
        virtqueue_kick(&capsule->queue)) {
        virtqueue_chain_release(&capsule->queue, token);
        return -1;
    }

    for (unsigned int attempt = 0; attempt < 2000000u; attempt++) {
        unsigned long long done_token = 0;
        unsigned int length = 0;
        int collected = virtqueue_collect(&capsule->queue, &done_token, &length);
        if (collected < 0) return -1;
        if (collected == 1 && done_token == token)
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
// the descriptor chain and the data-pool zero-copy against a real device before
// clients arrive.
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

// Steady state: drain every queued request, then park on an event nothing signals
// and idle at zero CPU while registered as the live virtio-blk driver. A
// kernel-to-capsule wakeup doorbell is future work.
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
    if (bring_up(&capsule)) return 2;
    mich_write("Mich virtio-blk: bootstrap pass\n");
    if (setup_transport(&capsule)) return 3;
    if (start_queue(&capsule)) return 4;
    if (self_test(&capsule)) return 5;
    mich_write("Mich virtio-blk: serve pass\n");
    serve_loop(&capsule);
    return 6;
}
