#include "types.h"
#include "object.h"
#include "resource.h"
#include "event.h"
#include "pci64.h"
#include "block.h"
#include "blockfs.h"
#include "vfs.h"
#include "nvme.h"
#include "kernel64_internal.h"
#include "tests64.h"
#include "test_report.h"
#include "serial64.h"

static int service_collect(struct kernel_object *dev, u64 id, i32 *status,
                           u32 *transferred) {
    for (u32 spin = 0; spin < 1000000u; spin++) {
        block_service(dev);
        if (!block_collect(dev, id, status, transferred, 0, 0)) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

int test_nvme64(const struct test64_env *env) {
    u32 objects = object_active_count();
    u32 blocks = block_active_count();
    u32 controllers = nvme_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    u32 mounts = vfs_mount_active_count();
    struct kernel_object *pci = 0;
    for (u32 index = 0; index < pci64_count(); index++) {
        struct kernel_object *candidate = pci64_object(index);
        const struct pci_resource *info = pci_resource_get(candidate);
        if (info && info->class_code == 0x01 && info->subclass == 0x08 &&
            info->programming_interface == 0x02) {
            pci = candidate;
            break;
        }
    }
    if (!pci) return 1;
    struct kernel_object *dev = nvme_open(pci);
    struct block_info info;
    int valid = dev && !nvme_open(pci) && !block_info(dev, &info) &&
        info.sector_size == BLOCK_SECTOR_SIZE && info.sector_count >= 64 &&
        (info.flags & BLOCK_FLAG_DEFER);
    u8 pattern[BLOCK_SECTOR_SIZE];
    u8 got[BLOCK_SECTOR_SIZE];
    for (u32 index = 0; index < sizeof(pattern); index++) {
        pattern[index] = (u8)(0xC3 ^ index);
        got[index] = 0;
    }
    valid = valid &&
        !block_io(dev, BLOCK_OP_WRITE, 8, 1, pattern, sizeof(pattern)) &&
        !block_io(dev, BLOCK_OP_READ, 8, 1, got, sizeof(got));
    for (u32 index = 0; index < sizeof(got); index++)
        if (got[index] != pattern[index]) valid = 0;
    for (u32 index = 0; index < sizeof(got); index++)
        if (got[index] != pattern[index]) valid = 0;
    struct kernel_object *pages = shared_memory_resource_create(2);
    struct kernel_object *sink = shared_memory_resource_create(2);
    valid = valid && pages && sink;
    struct kernel_object *sg = 0;
    struct kernel_object *sg_sink = 0;
    if (valid) {
        const struct page_resource *source = page_resource_get(pages);
        const struct page_resource *target = page_resource_get(sink);
        u8 *source_bytes[2] = {
            (u8 *)(uptr_t)source->physical[0],
            (u8 *)(uptr_t)source->physical[1],
        };
        u8 *target_bytes[2] = {
            (u8 *)(uptr_t)target->physical[0],
            (u8 *)(uptr_t)target->physical[1],
        };
        u8 stream[1536];
        for (u32 index = 0; index < sizeof(stream); index++) {
            stream[index] = (u8)(index * 7 + 11);
            u32 slot = index < 512 ? 0 : 1;
            u32 offset = index < 512 ? index + 512 : index - 512;
            if (source_bytes[slot])
                source_bytes[slot][offset] = stream[index];
            if (target_bytes[slot]) target_bytes[slot][offset] = 0;
        }
        struct kernel_object *sgl[3] = {pages, pages, pages};
        u32 indices[3] = {0, 1, 1};
        u32 offsets[3] = {512, 0, 512};
        u32 lengths[3] = {512, 512, 512};
        sg = sg_resource_create(sgl, indices, offsets, lengths, 3);
        u64 id = 0;
        i32 status = -1;
        u32 transferred = 0;
        valid = valid && sg &&
            !block_submit_sg(dev, BLOCK_OP_WRITE, 20, 3, sg, &id) &&
            !service_collect(dev, id, &status, &transferred) &&
            status == 0 && transferred == sizeof(stream);
        if (sg) {
            object_release(sg);
            sg = 0;
        }
        struct kernel_object *sgl_sink[3] = {sink, sink, sink};
        sg_sink = sg_resource_create(sgl_sink, indices, offsets, lengths, 3);
        valid = valid && sg_sink &&
            !block_submit_sg(dev, BLOCK_OP_READ, 20, 3, sg_sink, &id) &&
            !service_collect(dev, id, &status, &transferred) &&
            status == 0 && transferred == sizeof(stream);
        for (u32 index = 0; index < sizeof(stream); index++) {
            u32 slot = index < 512 ? 0 : 1;
            u32 offset = index < 512 ? index + 512 : index - 512;
            if (target_bytes[slot] &&
                target_bytes[slot][offset] != stream[index])
                valid = 0;
        }
        for (u32 sector = 0; sector < 3; sector++) {
            valid = valid && !block_io(dev, BLOCK_OP_READ, 20 + sector, 1,
                                       got, sizeof(got));
            for (u32 index = 0; index < sizeof(got); index++)
                if (got[index] != stream[sector * 512 + index]) valid = 0;
        }
        if (sg_sink) {
            object_release(sg_sink);
            sg_sink = 0;
        }
    }
    if (valid) {
        for (u32 index = 0; index < sizeof(pattern); index++)
            pattern[index] = (u8)(index ^ 0x5A);
        u64 id = 0;
        i32 status = -1;
        u32 transferred = 0;
        struct kernel_object *event = block_wait_event(dev);
        valid = valid && event && !event_reset(event) &&
            !block_submit(dev, BLOCK_OP_WRITE, 30, 1, pattern,
                          sizeof(pattern), &id);
        // Tests run before the scheduler starts, so a real block would
        // never wake: register, cancel, give the device time to fire the
        // MSI-X vector, then re-check the auto-reset event.
        // QEMU posts the completion entry and raises the MSI-X vector
        // synchronously while servicing the submission doorbell, so the
        // vector is already pending in the LAPIC when block_submit returns.
        // Ring 0 runs with interrupts disabled by kernel protocol (IF is
        // only set for user mode), so open a short window here to let the
        // pending vector through, then check the auto-reset event without
        // ever blocking on the scheduler.
        __asm__ volatile("sti");
        for (u32 spin = 0; spin < 5000000u; spin++)
            __asm__ volatile("pause");
        __asm__ volatile("cli");
        int woke = event_wait_timeout(event, env->target_slot,
                                      timer_ticks + 400);
        if (woke == 1) {
            event_cancel_task(env->target_slot, 0);
            woke = -110;
        }
        valid = valid && woke == 0 &&
            !service_collect(dev, id, &status, &transferred) &&
            status == 0 && transferred == sizeof(pattern);
    }
    struct kernel_object *root = vfs_root();
    struct kernel_object *mnt = root ?
        vfs_create(root, "nvmedisk", VFS_NODE_DIRECTORY) : 0;
    valid = valid && mnt && !blockfs_format(dev) &&
        !vfs_mount_blockfs(mnt, dev);
    struct kernel_object *disk = valid ? vfs_lookup(root, "nvmedisk") : 0;
    struct kernel_object *node = disk ?
        vfs_create_mode(disk, "hello", VFS_NODE_REGULAR, 0604) : 0;
    struct kernel_object *opened = node ? vfs_open(node) : 0;
    u8 payload[16];
    u8 received[16];
    u32 transferred = 0;
    for (u32 index = 0; index < sizeof(payload); index++) {
        payload[index] = (u8)('A' + index);
        received[index] = 0;
    }
    struct vfs_node_info details;
    valid = valid && disk && node && opened &&
        !vfs_write(opened, 0, payload, sizeof(payload), &transferred) &&
        transferred == sizeof(payload) &&
        !vfs_read(opened, 0, received, sizeof(received), &transferred) &&
        transferred == sizeof(received) &&
        !vfs_stat(opened, &details) &&
        details.filesystem == VFS_FILESYSTEM_BLOCKFS &&
        details.size == sizeof(payload) && details.mode == 0604;
    for (u32 index = 0; index < sizeof(received); index++)
        if (received[index] != payload[index]) valid = 0;
    if (opened) object_release(opened);
    if (node) object_release(node);
    valid = valid && disk && !vfs_unlink(disk, "hello");
    if (disk) object_release(disk);
    if (mnt) valid = valid && !vfs_unmount(mnt);
    if (mnt) object_release(mnt);
    u64 stale_id = 0;
    valid = valid && !block_revoke(dev) &&
        block_io(dev, BLOCK_OP_READ, 8, 1, got, sizeof(got)) < 0 &&
        block_submit_sg(dev, BLOCK_OP_READ, 20, 3, pages, &stale_id) < 0;
    if (mnt) object_release(mnt);
    if (root) object_release(root);
    if (sg_sink) object_release(sg_sink);
    if (sg) object_release(sg);
    if (sink) object_release(sink);
    if (pages) object_release(pages);
    if (dev) object_release(dev);
    valid = valid && object_active_count() == objects &&
        block_active_count() == blocks &&
        nvme_active_count() == controllers &&
        vfs_node_active_count() == nodes &&
        vfs_file_active_count() == files &&
        vfs_mount_active_count() == mounts;
    return valid ? 0 : -1;
}

// The kernel decides when NVMe can be probed; what a pass looks like, and
// which markers say so, belongs here with the rest of the tests.
int tests64_run_nvme(const struct test64_env *env) {
    int outcome = test_nvme64(env);
    // Negative is a failure, zero is a pass, and positive means the profile
    // has no NVMe device to probe: the unit image carries none, and that is
    // not an error.
    if (outcome < 0) return -1;
    if (test_report_record(TEST_ID_NVME, 0)) return -1;
    if (outcome) return 0;
    serial64_write("Mich test64: nvme controller and prp io pass\n");
    serial64_write("Mich test64: nvme scatter-gather io pass\n");
    serial64_write("Mich test64: nvme msi-x completion wake pass\n");
    serial64_write("Mich test64: nvme blockfs mount pass\n");
    return 0;
}
