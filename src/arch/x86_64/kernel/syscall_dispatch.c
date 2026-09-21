#include "types.h"
#include "bootinfo.h"
#include "version.h"
#include "vm64.h"
#include "elf64.h"
#include "task.h"
#include "arch_task.h"
#include "scheduler.h"
#include "service.h"
#include "capability.h"
#include "pmm.h"
#include "ipc64.h"
#include "protos.h"
#include "runtime64.h"
#include "gdt64.h"
#include "idt64.h"
#include "serial64.h"
#include "object.h"
#include "resource.h"
#include "acpi64.h"
#include "pci64.h"
#include "virtio_pci.h"
#include "virtio_abi.h"
#include "platform64.h"
#include "driver.h"
#include "driver_supervisor.h"
#include "driver_manager.h"
#include "event.h"
#include "endpoint.h"
#include "bridge.h"
#include "vector64.h"
#include "apic64.h"
#include "smp64.h"
#include "msi64.h"
#include "msix64.h"
#include "handle_transfer.h"
#include "panic64.h"
#include "irq_group.h"
#include "sg.h"
#include "ring.h"
#include "completion.h"
#include "completion_abi.h"
#include "timer_object.h"
#include "wait_many.h"
#include "net_buffer.h"
#include "vnic.h"
#include "vnic_benchmark.h"
#include "net_interface.h"
#include "net_interface_abi.h"
#include "net_abi.h"
#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "ipv6.h"
#include "icmp.h"
#include "icmpv6.h"
#include "udpv6.h"
#include "tcp.h"
#include "loopback.h"
#include "udp.h"
#include "route.h"
#include "socket.h"
#include "socket_abi.h"
#include "vfs.h"
#include "vfs_abi.h"
#include "firmware.h"
#include "firmware_abi.h"
#include "block.h"
#include "block_abi.h"
#include "virtio_blk.h"
#include "posix_abi.h"
#include "posix_fd.h"
#include "posix_profile.h"
#include "posix_vfs.h"
#include "kernel64_internal.h"

u64 syscall64_validate_return(u64 result) {
    struct smp64_syscall *sc = smp64_syscall();
    u32 slot = smp64_running_slot();
    u32 space = slot < MAX_TASKS ? task_contexts[slot].vm_space : 0;
    int rip_canonical = (sc->rip >> 47) == 0;
    int rsp_canonical = (sc->rsp >> 47) == 0;
    u64 rip_flags = rip_canonical
        ? vm64_user_flags(space, sc->rip) : 0;
    u64 stack_flags = rsp_canonical && sc->rsp > VM64_USER_BASE
        ? vm64_user_flags(space, sc->rsp - 1) : 0;
    int stack_writable = (stack_flags & VM64_PAGE_WRITE) ||
                         (stack_flags & VM64_PAGE_COW);
    if (!rip_canonical || !rsp_canonical ||
        (rip_flags & (VM64_PAGE_PRESENT | VM64_PAGE_USER | VM64_PAGE_NX)) !=
        (VM64_PAGE_PRESENT | VM64_PAGE_USER) ||
        (stack_flags & (VM64_PAGE_PRESENT | VM64_PAGE_USER)) !=
        (VM64_PAGE_PRESENT | VM64_PAGE_USER) ||
        !stack_writable) {
        serial64_write("Mich x86_64: invalid syscall return contained\n");
        // AP must not schedule through the BSP current_task_slot.
        if (smp64_cpu_index() != 0) {
            if (slot < MAX_TASKS)
                terminate64(slot, 141);
            smp64_set_current(SMP64_CURRENT_NONE);
            for (;;)
                __asm__ volatile("cli; hlt" ::: "memory");
        }
        fpu64_save(&task_contexts[current_task_slot]);
        terminate64(current_task_slot, 141);
        u32 next = scheduler64_next_slot();
        if (next == current_task_slot || task_pool[next].state != TASK_RUNNING) {
            serial64_write("Mich x86_64: no runnable task\n");
            for (;;) __asm__ volatile("cli; hlt");
        }
        scheduler64_set_running(next);
        fpu64_load(&task_contexts[current_task_slot]);
        context_load(&task_contexts[current_task_slot]);
        result = task_contexts[current_task_slot].rax;
    }
    sc->rflags = (sc->rflags & 0x8D5ULL) | 0x202ULL;
    return result;
}

static struct kernel_object *wait_event_for(struct kernel_object *object) {
    if (!object) return 0;
    if (object->type == KOBJECT_EVENT) return object;
    if (object->type == KOBJECT_COMPLETION)
        return completion_wait_event(object);
    if (object->type == KOBJECT_TIMER)
        return timer_object_wait_event(object);
    if (object->type == KOBJECT_ENDPOINT)
        return bridge_endpoint_wait_event(object);
    if (object->type == KOBJECT_SOCKET)
        return socket_wait_event(object);
    if (object->type == KOBJECT_NET_INTERFACE)
        return net_interface_wait_event(object);
    if (object->type == KOBJECT_BLOCK)
        return block_wait_event(object);
    return 0;
}

static void posix_stat_record(const struct vfs_node_info *info,
                              struct posix_stat_record *stat) {
    stat->st_mode = info->mode | (info->type == VFS_NODE_DIRECTORY ?
        0040000u : 0100000u);
    stat->st_size = info->size;
    stat->st_nlink = info->type == VFS_NODE_DIRECTORY ? info->child_count + 2 : 1;
    stat->st_reserved = 0;
}

static int posix_fd_error(struct task *task, int descriptor, u32 access) {
    int result = posix_fd_validate(task, descriptor, access);
    if (result == -1) return POSIX_VFS_EBADF;
    return result == -2 ? POSIX_VFS_EACCES : 0;
}

u64 syscall64_dispatch(u64 number, u64 arg0, u64 arg1, u64 arg2) {
    if (smp64_catch_ap_user(number)) {
        for (;;)
            __asm__ volatile("sti; hlt" ::: "memory");
    }
    if (number == 1) return (u64)serial64_user_write(arg0);
    if (number == 5) return (u64)(i64)ipc64_send((u32)arg0, arg1);
    if (number == 6) return (u64)(i64)ipc64_recv(0, arg0);
    if (number == 7) return (u64)(i64)ipc64_send_nb((u32)arg0, arg1);
    if (number == 24) return (u64)(i64)ipc64_recv((u32)arg0, arg1);
    if (number == 31) {
        if (!arg2 || arg2 > 0x7FFFFFFFULL) return (u64)(i64)EINVAL;
        return (u64)(i64)ipc64_send_timeout((u32)arg0, arg1,
                                            timer_ticks + (u32)arg2);
    }
    if (number == 11)
        return task_pool[current_task_slot].capabilities & CAP_RESOURCE_ADMIN ?
            vm64_available_pages() : (u64)(i64)EPERM;
    if (number == 12)
        return (u64)(i64)fork64();
    if (number == 13) {
        int result = exec64(arg0, arg1);
        if (result) return (u64)(i64)result;
        context_load(&task_contexts[current_task_slot]);
        return 0;
    }
    if (number == 14) {
        fpu64_save(&task_contexts[current_task_slot]);
        terminate64(current_task_slot, (int)arg0);
        u32 next = scheduler64_next_slot();
        if (next == current_task_slot || task_pool[next].state != TASK_RUNNING) {
            serial64_write("Mich x86_64: no runnable task\n");
            for (;;) __asm__ volatile("cli; hlt");
        }
        scheduler64_set_running(next);
        fpu64_load(&task_contexts[current_task_slot]);
        context_load(&task_contexts[current_task_slot]);
        return task_contexts[current_task_slot].rax;
    }
    if (number == 15) {
        int pid = (int)arg0;
        struct task *parent = &task_pool[current_task_slot];
        struct task *child = 0;
        if (pid == -1) {
            for (int index = 1; index < task_pool_count; index++) {
                struct task *candidate = &task_pool[index];
                if (candidate->state == TASK_FREE ||
                    candidate->parent_id != parent->id)
                    continue;
                if (candidate->state == TASK_ZOMBIE) {
                    int code;
                    if (task_reap_zombie(parent, candidate->id, &code) != 0)
                        return (u64)-1;
                    return (u64)(u32)code;
                }
                if (!child) child = candidate;
            }
            if (!child) return (u64)-1;
        } else {
            u32 slot = PID_SLOT((u32)pid);
            if (slot == 0 || slot >= (u32)task_pool_count) return (u64)-1;
            child = &task_pool[slot];
            if (child->state == TASK_FREE || child->id != pid ||
                child->parent_id != parent->id)
                return (u64)-1;
            if (child->state == TASK_ZOMBIE) {
                int code;
                if (task_reap_zombie(parent, pid, &code) != 0) return (u64)-1;
                return (u64)(u32)code;
            }
        }
        parent->wait_pid = pid;
        parent->state = TASK_BLOCKED_WAIT;
        if (scheduler_pick_next((int)current_task_slot) < 0) {
            parent->wait_pid = -1;
            parent->state = TASK_RUNNING;
            return (u64)(i64)EDEADLK;
        }
        scheduler64_switch();
        return task_contexts[current_task_slot].rax;
    }
    if (number == 16) {
        u32 slot = smp64_running_slot();
        if (slot >= MAX_TASKS) return (u64)-1;
        return (u64)(u32)task_pool[slot].id;
    }
    if (number == 17) {
        u32 slot = PID_SLOT((u32)arg0);
        struct task *owner = &task_pool[current_task_slot];
        if (slot == 0 || slot >= (u32)task_pool_count ||
            slot == current_task_slot)
            return (u64)-1;
        struct task *target = &task_pool[slot];
        if (target->state == TASK_FREE || target->state == TASK_ZOMBIE ||
            target->id != (int)arg0 ||
            (target->parent_id != owner->id &&
             !(owner->capabilities & CAP_TASK_ADMIN)))
            return (u64)-1;
        terminate64(slot, 137);
        return 0;
    }
    if (number == 18) return (u64)(i64)service_register((u32)arg0);
    if (number == 19) return (u64)(i64)service_lookup((u32)arg0);
    if (number == 20) return task_pool[current_task_slot].capabilities;
    if (number == 21) {
        task_pool[current_task_slot].capabilities &= ~(u32)arg0;
        return 0;
    }
    if (number == 22) {
        struct task *owner = &task_pool[current_task_slot];
        if (!(owner->capabilities & CAP_TASK_ADMIN) ||
            ((u32)arg1 & ~CAP_BOOT_ALLOWED) ||
            ((u32)arg1 & ~owner->capabilities))
            return (u64)-1;
        u32 slot = PID_SLOT((u32)arg0);
        if (slot == 0 || slot >= (u32)task_pool_count) return (u64)-1;
        struct task *target = &task_pool[slot];
        if (target->state == TASK_FREE || target->state == TASK_ZOMBIE ||
            target->id != (int)arg0)
            return (u64)-1;
        target->capabilities |= (u32)arg1;
        return 0;
    }
    if (number == 23) {
        scheduler64_switch();
        return task_contexts[current_task_slot].rax;
    }
    if (number == 32) {
        if (!(task_pool[current_task_slot].capabilities & CAP_TASK_ADMIN))
            return (u64)-1;
        fpu64_save(&task_contexts[current_task_slot]);
        int pid = spawn64(task_pool[current_task_slot].id, 0,
                          "spawn64", arg0);
        fpu64_load(&task_contexts[current_task_slot]);
        return (u64)(i64)pid;
    }
    if (number == 33)
        return (u64)(i64)handle_close(&task_pool[current_task_slot],
                                      (u32)arg0);
    if (number == 34) {
        struct kernel_object *object = event_create((u32)arg0, arg1 != 0);
        if (!object) return (u64)-1;
        u32 handle = handle_open(&task_pool[current_task_slot], object,
                                 KRIGHT_WAIT | KRIGHT_SIGNAL |
                                 KRIGHT_TRANSFER);
        object_release(object);
        return handle ? handle : (u64)-1;
    }
    if (number == 35) {
        struct kernel_object *object =
            handle_get(&task_pool[current_task_slot], (u32)arg0,
                       KRIGHT_WAIT, KOBJECT_EVENT);
        if (!object) return (u64)-1;
        int result = event_wait(object, current_task_slot);
        if (result <= 0) return (u64)(i64)result;
        scheduler64_switch();
        return task_contexts[current_task_slot].rax;
    }
    if (number == 36) {
        struct kernel_object *object =
            handle_get(&task_pool[current_task_slot], (u32)arg0,
                       KRIGHT_SIGNAL, KOBJECT_EVENT);
        return object ? (u64)(i64)event_signal(object) : (u64)-1;
    }
    if (number == 37) {
        struct kernel_object *object =
            handle_get(&task_pool[current_task_slot], (u32)arg0,
                       KRIGHT_SIGNAL, KOBJECT_EVENT);
        return object ? (u64)(i64)event_reset(object) : (u64)-1;
    }
    if (number == 38) {
        u32 slot = PID_SLOT((u32)arg0);
        struct task *source = &task_pool[current_task_slot];
        if (slot == 0 || slot >= (u32)task_pool_count) return (u64)-1;
        struct task *target = &task_pool[slot];
        if (target->state == TASK_FREE || target->state == TASK_ZOMBIE ||
            target->id != (int)arg0 ||
            (target->parent_id != source->id &&
             !(source->capabilities & CAP_TASK_ADMIN)))
            return (u64)-1;
        u32 handle = handle_duplicate(source, target, (u32)arg1, (u32)arg2);
        return handle ? handle : (u64)-1;
    }
    if (number == 39) {
        struct kernel_object *endpoint = bridge_endpoint_create();
        if (!endpoint) return (u64)-1;
        u32 handle = handle_open(&task_pool[current_task_slot], endpoint,
                                 KRIGHT_READ | KRIGHT_WAIT |
                                 KRIGHT_TRANSFER);
        object_release(endpoint);
        return handle ? handle : (u64)-1;
    }
    if (number == 40 || number == 45) {
        struct kernel_object *endpoint =
            handle_get(&task_pool[current_task_slot], (u32)arg0,
                       KRIGHT_WAIT, KOBJECT_ENDPOINT);
        if (!endpoint) return (u64)-1;
        int result = bridge_endpoint_wait(endpoint, current_task_slot);
        if (result <= 0) return (u64)(i64)result;
        scheduler64_switch();
        return task_contexts[current_task_slot].rax;
    }
    if (number == 41) {
        struct kernel_object *endpoint =
            handle_get(&task_pool[current_task_slot], (u32)arg0,
                       KRIGHT_READ, KOBJECT_ENDPOINT);
        if (!endpoint ||
            vm64_user_access(task_pool[current_task_slot].page_dir, arg1,
                             sizeof(struct bridge_notification), 1))
            return (u64)-1;
        struct bridge_notification notification;
        if (bridge_endpoint_read(endpoint, &notification)) return (u64)-1;
        return (u64)(i64)vm64_copy_to(task_pool[current_task_slot].page_dir,
                                      arg1, &notification,
                                      sizeof(notification));
    }
    if (number == 42) {
        struct kernel_object *irq =
            handle_get(&task_pool[current_task_slot], (u32)arg0,
                       KRIGHT_CONTROL, KOBJECT_IRQ);
        struct kernel_object *endpoint =
            handle_get(&task_pool[current_task_slot], (u32)arg1,
                       KRIGHT_WAIT, KOBJECT_ENDPOINT);
        return irq && endpoint ? (u64)(i64)irq_resource_bind(irq, endpoint)
                               : (u64)-1;
    }
    if (number == 43 || number == 44) {
        struct kernel_object *irq =
            handle_get(&task_pool[current_task_slot], (u32)arg0,
                       KRIGHT_CONTROL, KOBJECT_IRQ);
        if (!irq) return (u64)-1;
        return number == 43 ? (u64)(i64)irq_resource_unbind(irq)
                            : (u64)(i64)irq_resource_set_mask(irq, arg1 != 0);
    }
    if (number >= 46 && number <= 53 &&
        !(task_pool[current_task_slot].capabilities & CAP_RESOURCE_ADMIN))
        return (u64)-1;
    if (number == 46) return pci64_count();
    if (number == 47) {
        struct kernel_object *pci = pci64_object((u32)arg0);
        if (!pci) return (u64)-1;
        u32 handle = handle_open(&task_pool[current_task_slot], pci,
                                 KRIGHT_READ | KRIGHT_CONTROL |
                                 KRIGHT_TRANSFER);
        return handle ? handle : (u64)-1;
    }
    if (number == 48) {
        struct kernel_object *pci =
            handle_get(&task_pool[current_task_slot], (u32)arg0,
                       KRIGHT_READ, KOBJECT_PCI);
        struct kernel_object *mmio =
            pci ? pci64_bar_create(pci, (u32)arg1) : 0;
        if (!mmio) return (u64)-1;
        u32 handle = handle_open(&task_pool[current_task_slot], mmio,
                                 KRIGHT_READ | KRIGHT_WRITE | KRIGHT_MAP |
                                 KRIGHT_TRANSFER);
        object_release(mmio);
        return handle ? handle : (u64)-1;
    }
    if (number == 49) {
        struct kernel_object *dma =
            dma_resource_allocate((u32)arg0, (paddr_t)arg1);
        if (!dma) return (u64)-1;
        u32 handle = handle_open(&task_pool[current_task_slot], dma,
                                 KRIGHT_READ | KRIGHT_WRITE | KRIGHT_MAP |
                                 KRIGHT_TRANSFER);
        object_release(dma);
        return handle ? handle : (u64)-1;
    }
    if (number == 50) {
        struct kernel_object *pci =
            handle_get(&task_pool[current_task_slot], (u32)arg0,
                       KRIGHT_CONTROL, KOBJECT_PCI);
        struct kernel_object *irq = pci ? msi64_create(pci) : 0;
        if (!irq) return (u64)-1;
        u32 handle = handle_open(&task_pool[current_task_slot], irq,
                                 KRIGHT_CONTROL | KRIGHT_WAIT |
                                 KRIGHT_TRANSFER);
        object_release(irq);
        return handle ? handle : (u64)-1;
    }
    if (number == 51) {
        struct kernel_object *pci =
            handle_get(&task_pool[current_task_slot], (u32)arg0,
                       KRIGHT_CONTROL, KOBJECT_PCI);
        struct kernel_object *table = pci ? pci64_msix_table_create(pci) : 0;
        if (!table) return (u64)-1;
        u32 handle = handle_open(&task_pool[current_task_slot], table,
                                 KRIGHT_READ | KRIGHT_CONTROL | KRIGHT_MAP |
                                 KRIGHT_TRANSFER);
        object_release(table);
        return handle ? handle : (u64)-1;
    }
    if (number == 52) {
        struct kernel_object *irq = platform64_irq_object((u32)arg0);
        if (!irq) return (u64)-1;
        u32 handle = handle_open(&task_pool[current_task_slot], irq,
                                 KRIGHT_CONTROL | KRIGHT_WAIT |
                                 KRIGHT_TRANSFER);
        return handle ? handle : (u64)-1;
    }
    if (number == 53) {
        struct kernel_object *table =
            handle_get(&task_pool[current_task_slot], (u32)arg0,
                       KRIGHT_CONTROL, KOBJECT_MSIX_TABLE);
        struct kernel_object *irq = table ? msix64_create(table, (u32)arg1) : 0;
        if (!irq) return (u64)-1;
        u32 handle = handle_open(&task_pool[current_task_slot], irq,
                                 KRIGHT_CONTROL | KRIGHT_WAIT |
                                 KRIGHT_TRANSFER);
        object_release(irq);
        return handle ? handle : (u64)-1;
    }
    if (number == 54) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *mmio =
            handle_get(task, (u32)arg0, KRIGHT_READ | KRIGHT_MAP,
                       KOBJECT_MMIO);
        const struct mmio_resource *resource = mmio_resource_get(mmio);
        if (!resource) return (u64)-1;
        int writable = handle_get(task, (u32)arg0, KRIGHT_WRITE,
                                  KOBJECT_MMIO) != 0;
        return (u64)(i64)vm64_map_object(
            task_contexts[current_task_slot].vm_space, arg1, mmio,
            resource->physical, resource->length, writable,
            resource->cache_mode);
    }
    if (number == 55) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *dma =
            handle_get(task, (u32)arg0, KRIGHT_READ | KRIGHT_MAP,
                       KOBJECT_DMA);
        const struct dma_resource *resource = dma_resource_get(dma);
        if (!resource) return (u64)-1;
        int writable = handle_get(task, (u32)arg0, KRIGHT_WRITE,
                                  KOBJECT_DMA) != 0;
        return (u64)(i64)vm64_map_object(
            task_contexts[current_task_slot].vm_space, arg1, dma,
            resource->physical, (usize_t)resource->pages * 4096,
            writable, VM64_CACHE_WB);
    }
    if (number == 56)
        return (u64)(i64)vm64_unmap_object(
            task_contexts[current_task_slot].vm_space, arg0, arg1);
    if (number == 57) {
        struct kernel_object *object =
            handle_get(&task_pool[current_task_slot], (u32)arg0,
                       KRIGHT_READ, KOBJECT_NONE);
        if (!object) return (u64)-1;
        if (object->type == KOBJECT_MMIO) {
            const struct mmio_resource *resource = mmio_resource_get(object);
            return resource ? resource->length : (u64)-1;
        }
        if (object->type == KOBJECT_DMA) {
            const struct dma_resource *resource = dma_resource_get(object);
            return resource ? (u64)resource->pages * 4096 : (u64)-1;
        }
        if (object->type == KOBJECT_MSIX_TABLE) {
            const struct msix_table_resource *resource =
                msix_table_resource_get(object);
            return resource ? (u64)resource->entries * 16 : (u64)-1;
        }
        if (object->type == KOBJECT_PAGE ||
            object->type == KOBJECT_SHARED_MEMORY) {
            const struct page_resource *resource = page_resource_get(object);
            return resource ? (u64)resource->pages * 4096 : (u64)-1;
        }
        if (object->type == KOBJECT_SG_LIST) {
            const struct sg_resource *resource = sg_resource_get(object);
            return resource ? resource->total_length : (u64)-1;
        }
        if (object->type == KOBJECT_RING) {
            const struct ring_resource *resource = ring_resource_get(object);
            return resource ? (u64)resource->pages * 4096 : (u64)-1;
        }
        if (object->type == KOBJECT_VIRTQUEUE) {
            struct kernel_object *dma = virtqueue_dma_object(object);
            const struct dma_resource *resource = dma_resource_get(dma);
            return resource ? (u64)resource->pages * 4096 : (u64)-1;
        }
        return (u64)-1;
    }
    if (number == 58) {
        if (!arg1 || arg1 > 0x7FFFFFFFULL) return (u64)(i64)EINVAL;
        struct kernel_object *object =
            handle_get(&task_pool[current_task_slot], (u32)arg0,
                       KRIGHT_WAIT, KOBJECT_EVENT);
        if (!object) return (u64)-1;
        int result = event_wait_timeout(object, current_task_slot,
                                        timer_ticks + (u32)arg1);
        if (result <= 0) return (u64)(i64)result;
        scheduler64_switch();
        return task_contexts[current_task_slot].rax;
    }
    if (number == 59) {
        u32 slot = PID_SLOT((u32)arg0);
        struct task *source = &task_pool[current_task_slot];
        if (slot == 0 || slot >= (u32)task_pool_count ||
            !arg2 || arg2 > HANDLE_TRANSFER_MAX)
            return (u64)-1;
        struct task *target = &task_pool[slot];
        if (target->state == TASK_FREE || target->state == TASK_ZOMBIE ||
            target->id != (int)arg0 ||
            (target->parent_id != source->id &&
             !(source->capabilities & CAP_TASK_ADMIN)))
            return (u64)-1;
        struct handle_transfer_entry entries[HANDLE_TRANSFER_MAX];
        usize_t bytes = (usize_t)arg2 * sizeof(entries[0]);
        if (vm64_copy_from(source->page_dir, entries, arg1, bytes))
            return (u64)-1;
        u32 completed = 0;
        while (completed < (u32)arg2) {
            u32 handle = handle_duplicate(source, target,
                                          entries[completed].source_handle,
                                          entries[completed].rights);
            if (!handle) break;
            entries[completed].target_handle = handle;
            completed++;
        }
        if (completed != (u32)arg2 ||
            vm64_copy_to(source->page_dir, arg1, entries, bytes)) {
            for (u32 index = 0; index < completed; index++)
                handle_close(target, entries[index].target_handle);
            return (u64)-1;
        }
        return 0;
    }
    if (number == 61 || number == 62) {
        struct irq_group_request request;
        struct task *task = &task_pool[current_task_slot];
        if (vm64_copy_from(task->page_dir, &request, arg0,
                           sizeof(request)) ||
            !request.count || request.count > IRQ_GROUP_MAX ||
            request.reserved)
            return (u64)-1;
        struct kernel_object *source = handle_get(
            task, request.resource_handle, KRIGHT_CONTROL,
            number == 61 ? KOBJECT_PCI : KOBJECT_MSIX_TABLE);
        if (!source) return (u64)-1;
        struct kernel_object *irqs[IRQ_GROUP_MAX];
        for (u32 index = 0; index < IRQ_GROUP_MAX; index++) irqs[index] = 0;
        int created = number == 61
            ? msi64_create_group(source, request.count, irqs, IRQ_GROUP_MAX)
            : msix64_create_group(source, request.first, request.count,
                                  irqs, IRQ_GROUP_MAX);
        if (created) return (u64)-1;
        u32 opened = 0;
        while (opened < request.count) {
            request.handles[opened] = handle_open(
                task, irqs[opened], KRIGHT_CONTROL | KRIGHT_WAIT |
                KRIGHT_TRANSFER);
            if (!request.handles[opened]) break;
            opened++;
        }
        for (u32 index = 0; index < request.count; index++)
            object_release(irqs[index]);
        if (opened != request.count ||
            vm64_copy_to(task->page_dir, arg0, &request, sizeof(request))) {
            while (opened) handle_close(task, request.handles[--opened]);
            return (u64)-1;
        }
        return 0;
    }
    if (number == 63 || number == 64) {
        struct kernel_object *object = number == 63
            ? page_resource_create()
            : shared_memory_resource_create((u32)arg0);
        if (!object) return (u64)-1;
        u32 handle = handle_open(
            &task_pool[current_task_slot], object,
            KRIGHT_READ | KRIGHT_WRITE | KRIGHT_MAP | KRIGHT_CONTROL |
            KRIGHT_TRANSFER);
        object_release(object);
        return handle ? handle : (u64)-1;
    }
    if (number == 65) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *object = handle_get(
            task, (u32)arg0, KRIGHT_READ | KRIGHT_MAP, KOBJECT_NONE);
        if (!object || (object->type != KOBJECT_PAGE &&
                        object->type != KOBJECT_SHARED_MEMORY))
            return (u64)-1;
        int writable = handle_get(task, (u32)arg0, KRIGHT_WRITE,
                                  object->type) != 0;
        return (u64)(i64)vm64_map_page_object(
            task_contexts[current_task_slot].vm_space, arg1,
            object, writable);
    }
    if (number >= 66 && number <= 68) {
        struct kernel_object *object = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_NONE);
        if (!object || (object->type != KOBJECT_PAGE &&
                        object->type != KOBJECT_SHARED_MEMORY))
            return (u64)-1;
        if (number == 66) return (u64)(i64)page_resource_pin(object);
        if (number == 67) return (u64)(i64)page_resource_unpin(object);
        return (u64)(i64)page_resource_revoke(object);
    }
    if (number == 69) {
        struct sg_create_request request;
        struct task *task = &task_pool[current_task_slot];
        if (vm64_copy_from(task->page_dir, &request, arg0,
                           sizeof(request)) ||
            !request.entry_count || request.entry_count > SG_USER_ENTRY_MAX ||
            request.reserved)
            return (u64)-1;
        struct kernel_object *pages[SG_USER_ENTRY_MAX];
        u32 page_indices[SG_USER_ENTRY_MAX];
        u32 offsets[SG_USER_ENTRY_MAX];
        u32 lengths[SG_USER_ENTRY_MAX];
        for (u32 index = 0; index < request.entry_count; index++) {
            pages[index] = handle_get(
                task, request.entries[index].page_handle,
                KRIGHT_READ | KRIGHT_CONTROL, KOBJECT_NONE);
            if (!pages[index] ||
                (pages[index]->type != KOBJECT_PAGE &&
                 pages[index]->type != KOBJECT_SHARED_MEMORY))
                return (u64)-1;
            page_indices[index] = request.entries[index].page_index;
            offsets[index] = request.entries[index].offset;
            lengths[index] = request.entries[index].length;
        }
        struct kernel_object *sg = sg_resource_create(
            pages, page_indices, offsets, lengths, request.entry_count);
        if (!sg) return (u64)-1;
        u32 handle = handle_open(
            task, sg, KRIGHT_READ | KRIGHT_CONTROL | KRIGHT_TRANSFER);
        object_release(sg);
        return handle ? handle : (u64)-1;
    }
    if (number == 70) {
        struct kernel_object *sg = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_SG_LIST);
        return sg ? (u64)(i64)sg_resource_revoke(sg) : (u64)-1;
    }
    if (number == 71) {
        struct kernel_object *ring =
            ring_resource_create((u32)arg0, (u32)arg1);
        if (!ring) return (u64)-1;
        u32 handle = handle_open(
            &task_pool[current_task_slot], ring,
            KRIGHT_READ | KRIGHT_WRITE | KRIGHT_MAP | KRIGHT_CONTROL |
            KRIGHT_TRANSFER);
        object_release(ring);
        return handle ? handle : (u64)-1;
    }
    if (number == 72) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *ring = handle_get(
            task, (u32)arg0, KRIGHT_READ | KRIGHT_MAP, KOBJECT_RING);
        struct kernel_object *backing = ring_resource_backing(ring);
        if (!backing) return (u64)-1;
        int writable = handle_get(task, (u32)arg0, KRIGHT_WRITE,
                                  KOBJECT_RING) != 0;
        return (u64)(i64)vm64_map_page_object(
            task_contexts[current_task_slot].vm_space, arg1,
            backing, writable);
    }
    if (number == 73 || number == 74) {
        struct kernel_object *ring = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_RING);
        if (!ring) return (u64)-1;
        return number == 73
            ? (u64)(i64)ring_resource_submit(ring, (u32)arg1)
            : (u64)(i64)ring_resource_consume(ring, (u32)arg1);
    }
    if (number == 75) {
        struct kernel_object *ring = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_RING);
        return ring ? (u64)(i64)ring_resource_revoke(ring) : (u64)-1;
    }
    if (number == 76) {
        struct kernel_object *completion = completion_create();
        if (!completion) return (u64)-1;
        u32 handle = handle_open(
            &task_pool[current_task_slot], completion,
            KRIGHT_READ | KRIGHT_WAIT | KRIGHT_SIGNAL | KRIGHT_CONTROL |
            KRIGHT_TRANSFER);
        object_release(completion);
        return handle ? handle : (u64)-1;
    }
    if (number == 77) {
        struct kernel_object *completion = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_COMPLETION);
        if (!completion || arg1 > 0x7FFFFFFFULL) return 0;
        return completion_request_begin(
            completion, timer_ticks + (u32)arg1, arg1 != 0);
    }
    if (number == 78) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *completion = handle_get(
            task, (u32)arg0, KRIGHT_SIGNAL, KOBJECT_COMPLETION);
        struct completion_update update;
        if (!completion || vm64_copy_from(task->page_dir, &update, arg1,
                                          sizeof(update)))
            return (u64)-1;
        return (u64)(i64)completion_request_finish(
            completion, update.id, update.status, update.transferred);
    }
    if (number == 79) {
        struct kernel_object *completion = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_COMPLETION);
        return completion ? (u64)(i64)completion_request_cancel(
            completion, arg1) : (u64)-1;
    }
    if (number == 80) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *completion = handle_get(
            task, (u32)arg0, KRIGHT_READ, KOBJECT_COMPLETION);
        struct completion_poll poll;
        if (!completion || vm64_copy_from(task->page_dir, &poll, arg1,
                                          sizeof(poll)) || poll.reserved)
            return (u64)-1;
        struct completion_result result;
        if (completion_request_poll(completion, poll.id, &result))
            return (u64)-1;
        poll.id = result.id;
        poll.status = result.status;
        poll.transferred = result.transferred;
        poll.state = result.state;
        poll.reserved = 0;
        return (u64)(i64)vm64_copy_to(task->page_dir, arg1, &poll,
                                      sizeof(poll));
    }
    if (number == 81 || number == 85) {
        struct kernel_object *object = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_WAIT, number == 81 ? KOBJECT_COMPLETION : KOBJECT_TIMER);
        struct kernel_object *event = wait_event_for(object);
        if (!event) return (u64)-1;
        int result = event_wait(event, current_task_slot);
        if (result <= 0) return (u64)(i64)result;
        scheduler64_switch();
        return task_contexts[current_task_slot].rax;
    }
    if (number == 82) {
        struct kernel_object *timer = timer_object_create();
        if (!timer) return (u64)-1;
        u32 handle = handle_open(
            &task_pool[current_task_slot], timer,
            KRIGHT_WAIT | KRIGHT_CONTROL | KRIGHT_TRANSFER);
        object_release(timer);
        return handle ? handle : (u64)-1;
    }
    if (number == 83) {
        struct kernel_object *timer = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_TIMER);
        if (!timer || !arg1 || arg1 > 0x7FFFFFFFULL ||
            arg2 > 0x7FFFFFFFULL)
            return (u64)-1;
        return (u64)(i64)timer_object_arm(
            timer, timer_ticks + (u32)arg1, (u32)arg2);
    }
    if (number == 84) {
        struct kernel_object *timer = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_TIMER);
        return timer ? (u64)(i64)timer_object_cancel(timer) : (u64)-1;
    }
    if (number == 86) {
        struct task *task = &task_pool[current_task_slot];
        struct wait_many_request request;
        if (vm64_copy_from(task->page_dir, &request, arg0,
                           sizeof(request)) || !request.count ||
            request.count > WAIT_MANY_MAX ||
            request.timeout > 0x7FFFFFFFu)
            return (u64)-1;
        struct kernel_object *events[WAIT_MANY_MAX];
        for (u32 index = 0; index < request.count; index++) {
            struct kernel_object *object = handle_get(
                task, request.handles[index], KRIGHT_WAIT, KOBJECT_NONE);
            events[index] = wait_event_for(object);
            if (!events[index]) return (u64)-1;
        }
        u32 ready = 0;
        int result = event_wait_many(
            events, request.count, current_task_slot,
            timer_ticks + request.timeout, request.timeout != 0, &ready);
        if (result < 0) return (u64)-1;
        if (!result) return ready;
        scheduler64_switch();
        return task_contexts[current_task_slot].rax;
    }
    if (number == 87) {
        struct task *task = &task_pool[current_task_slot];
        struct vnic_create_request request;
        if (vm64_copy_from(task->page_dir, &request, arg0,
                           sizeof(request)))
            return (u64)-1;
        struct kernel_object *vnic = vnic_create(
            request.buffer_count, request.ring_capacity);
        struct kernel_object *pool = vnic_pool(vnic);
        struct kernel_object *rx = vnic_rx_ring(vnic);
        struct kernel_object *tx = vnic_tx_ring(vnic);
        if (!vnic || !pool || !rx || !tx) {
            if (vnic) object_release(vnic);
            return (u64)-1;
        }
        u32 handles[4];
        handles[0] = handle_open(task, vnic,
                                 KRIGHT_READ | KRIGHT_CONTROL);
        handles[1] = handle_open(task, pool,
                                 KRIGHT_READ | KRIGHT_WRITE | KRIGHT_MAP |
                                 KRIGHT_CONTROL);
        handles[2] = handle_open(task, rx,
                                 KRIGHT_READ | KRIGHT_WRITE | KRIGHT_MAP |
                                 KRIGHT_CONTROL);
        handles[3] = handle_open(task, tx,
                                 KRIGHT_READ | KRIGHT_WRITE | KRIGHT_MAP |
                                 KRIGHT_CONTROL);
        object_release(vnic);
        u32 opened = 0;
        for (u32 index = 0; index < 4; index++)
            if (handles[index]) opened++;
        if (opened != 4) {
            for (u32 index = 0; index < 4; index++)
                if (handles[index]) handle_close(task, handles[index]);
            return (u64)-1;
        }
        request.vnic_handle = handles[0];
        request.pool_handle = handles[1];
        request.rx_ring_handle = handles[2];
        request.tx_ring_handle = handles[3];
        if (vm64_copy_to(task->page_dir, arg0, &request, sizeof(request))) {
            while (opened) handle_close(task, handles[--opened]);
            return (u64)-1;
        }
        return 0;
    }
    if (number == 88) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *pool = handle_get(
            task, (u32)arg0, KRIGHT_READ | KRIGHT_MAP, KOBJECT_PACKET_POOL);
        struct kernel_object *backing = packet_pool_backing(pool);
        if (!backing) return (u64)-1;
        int writable = handle_get(task, (u32)arg0, KRIGHT_WRITE,
                                  KOBJECT_PACKET_POOL) != 0;
        return (u64)(i64)vm64_map_page_object(
            task_contexts[current_task_slot].vm_space, arg1,
            backing, writable);
    }
    if (number == 89) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *vnic = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_VNIC);
        u8 frame[NET_PACKET_DATA_MAX];
        if (!vnic || !arg2 || arg2 > NET_PACKET_DATA_MAX - NET_PACKET_HEADROOM ||
            vm64_copy_from(task->page_dir, frame, arg1, arg2))
            return (u64)-1;
        return (u64)(i64)vnic_inject(vnic, frame, (u32)arg2);
    }
    if (number == 90 || number == 94) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *vnic = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_VNIC);
        struct net_packet_descriptor descriptor;
        int result = number == 90 ? vnic_receive(vnic, &descriptor)
                                  : vnic_drain_tx(vnic, &descriptor);
        if (result) return (u64)-1;
        if (vm64_copy_to(task->page_dir, arg1, &descriptor,
                         sizeof(descriptor))) {
            if (number == 90) vnic_release_rx(vnic, descriptor.buffer_id);
            return (u64)-1;
        }
        return 0;
    }
    if (number == 91) {
        struct kernel_object *vnic = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_VNIC);
        return vnic ? (u64)(i64)vnic_release_rx(vnic, arg1) : (u64)-1;
    }
    if (number == 92) {
        struct kernel_object *vnic = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_VNIC);
        return vnic ? vnic_acquire_tx(vnic) : 0;
    }
    if (number == 93) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *vnic = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_VNIC);
        struct vnic_frame_request request;
        if (!vnic || vm64_copy_from(task->page_dir, &request, arg1,
                                    sizeof(request)))
            return (u64)-1;
        return (u64)(i64)vnic_submit_tx(
            vnic, request.buffer_id, request.offset,
            request.length, request.flags);
    }
    if (number == 95) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *vnic = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_VNIC);
        struct vnic_benchmark_request request;
        if (!vnic || vm64_copy_from(task->page_dir, &request, arg1,
                                    sizeof(request)) ||
            vnic_benchmark_run(vnic, &request))
            return (u64)-1;
        return (u64)(i64)vm64_copy_to(task->page_dir, arg1, &request,
                                      sizeof(request));
    }
    if (number == 96) {
        struct kernel_object *vnic = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_VNIC);
        return vnic ? (u64)(i64)vnic_revoke(vnic) : (u64)-1;
    }
    if (number == 97) {
        struct kernel_object *socket = socket_create();
        if (!socket) return (u64)-1;
        u32 handle = handle_open(
            &task_pool[current_task_slot], socket,
            KRIGHT_READ | KRIGHT_WRITE | KRIGHT_WAIT | KRIGHT_CONTROL |
            KRIGHT_TRANSFER);
        object_release(socket);
        return handle ? handle : (u64)-1;
    }
    if (number == 98) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *socket = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_SOCKET);
        struct socket_bind_request request;
        if (!socket || vm64_user_access(task->page_dir, arg1,
                                        sizeof(request), 1) ||
            vm64_copy_from(task->page_dir, &request, arg1,
                           sizeof(request)) || request.reserved ||
            socket_bind(socket, request.address, request.port))
            return (u64)-1;
        if (socket_local_address(socket, &request.address, &request.port))
            return (u64)-1;
        request.reserved = 0;
        return (u64)(i64)vm64_copy_to(task->page_dir, arg1, &request,
                                      sizeof(request));
    }
    if (number == 99) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *socket = handle_get(
            task, (u32)arg0, KRIGHT_WRITE, KOBJECT_SOCKET);
        struct socket_send_request request;
        if (!socket || vm64_copy_from(task->page_dir, &request, arg1,
                                      sizeof(request)) ||
            request.length > SOCKET_PAYLOAD_MAX)
            return (u64)-1;
        return (u64)(i64)socket_send_to(
            socket, request.destination_address, request.destination_port,
            request.payload, request.length);
    }
    if (number == 100) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *socket = handle_get(
            task, (u32)arg0, KRIGHT_READ, KOBJECT_SOCKET);
        struct udp_datagram datagram;
        if (!socket || vm64_user_access(task->page_dir, arg1,
                                        sizeof(struct socket_receive_result), 1) ||
            socket_receive_from(socket, &datagram))
            return (u64)-1;
        struct socket_receive_result result;
        result.source_address = datagram.source_address;
        result.destination_address = datagram.destination_address;
        result.source_port = datagram.source_port;
        result.destination_port = datagram.destination_port;
        result.length = datagram.length;
        result.reserved = 0;
        for (u32 index = 0; index < SOCKET_PAYLOAD_MAX; index++)
            result.payload[index] = index < datagram.length
                ? datagram.payload[index] : 0;
        return (u64)(i64)vm64_copy_to(task->page_dir, arg1, &result,
                                      sizeof(result));
    }
    if (number == 101) {
        struct kernel_object *socket = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_WAIT, KOBJECT_SOCKET);
        struct kernel_object *event = socket_wait_event(socket);
        if (!event) return (u64)-1;
        int result = event_wait(event, current_task_slot);
        if (result <= 0) return (u64)(i64)result;
        scheduler64_switch();
        return task_contexts[current_task_slot].rax;
    }
    if (number == 102) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct net_interface_create_request request;
        if (!domain || vm64_copy_from(task->page_dir, &request, arg0,
                                      sizeof(request)) ||
            request.reserved0[0] || request.reserved0[1] || request.reserved1)
            return (u64)-1;
        struct kernel_object *pool = handle_get(
            task, request.pool_handle, KRIGHT_CONTROL, KOBJECT_PACKET_POOL);
        struct kernel_object *rx = handle_get(
            task, request.rx_ring_handle, KRIGHT_CONTROL, KOBJECT_RING);
        struct kernel_object *tx = handle_get(
            task, request.tx_ring_handle, KRIGHT_CONTROL, KOBJECT_RING);
        struct kernel_object *interface = pool && rx && tx ?
            net_interface_create(domain, pool, rx, tx, request.mac,
                                 request.mtu, request.name) : 0;
        if (!interface || net_interface_register(interface)) {
            if (interface) object_release(interface);
            return (u64)-1;
        }
        struct net_interface *info = net_interface_get(interface);
        u32 handle = handle_open(
            task, interface, KRIGHT_READ | KRIGHT_WAIT | KRIGHT_CONTROL |
            KRIGHT_TRANSFER);
        request.interface_handle = handle;
        request.interface_id = info ? info->interface_id : 0;
        request.generation = info ? info->generation : 0;
        if (!handle || vm64_copy_to(task->page_dir, arg0, &request,
                                    sizeof(request))) {
            net_interface_remove(interface);
            if (handle) handle_close(task, handle);
            object_release(interface);
            return (u64)-1;
        }
        object_release(interface);
        return 0;
    }
    if (number == 103) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        return interface && domain ? (u64)(i64)net_interface_set_link(
            interface, domain, arg1 != 0) : (u64)-1;
    }
    if (number == 104) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *object = handle_get(
            task, (u32)arg0, KRIGHT_READ, KOBJECT_NET_INTERFACE);
        struct net_interface *interface = net_interface_get(object);
        if (!interface) return (u64)-1;
        struct net_interface_info info;
        info.interface_id = interface->interface_id;
        info.generation = interface->generation;
        info.state = interface->state;
        info.mtu = interface->mtu;
        info.ipv4_address = interface->ipv4_address;
        info.ipv4_netmask = interface->ipv4_netmask;
        for (u32 index = 0; index < 6; index++) info.mac[index] = interface->mac[index];
        info.reserved[0] = 0;
        info.reserved[1] = 0;
        for (u32 index = 0; index < NET_INTERFACE_ABI_NAME_MAX; index++)
            info.name[index] = interface->name[index];
        return (u64)(i64)vm64_copy_to(task->page_dir, arg1, &info, sizeof(info));
    }
    if (number == 105) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        if (!interface || !domain ||
            net_interface_set_link(interface, domain, 0))
            return (u64)-1;
        return (u64)(i64)net_interface_revoke(interface);
    }
    if (number == 106) {
        struct kernel_object *pci = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_PCI);
        struct kernel_object *device = pci ? virtio_pci_create(pci) : 0;
        if (!device) return (u64)-1;
        u32 handle = handle_open(
            &task_pool[current_task_slot], device,
            KRIGHT_READ | KRIGHT_CONTROL | KRIGHT_TRANSFER);
        object_release(device);
        return handle ? handle : (u64)-1;
    }
    if (number == 107) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *device = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_VIRTIO_DEVICE);
        struct virtio_feature_request request;
        if (!device || vm64_user_access(task->page_dir, arg1,
                                        sizeof(request), 1) ||
            vm64_copy_from(task->page_dir, &request, arg1,
                           sizeof(request)) ||
            virtio_pci_negotiate(device, request.wanted, request.required))
            return (u64)-1;
        struct virtio_device_info *info = virtio_pci_get(device);
        request.device_features = info->device_features;
        request.driver_features = info->driver_features;
        return (u64)(i64)vm64_copy_to(task->page_dir, arg1, &request,
                                      sizeof(request));
    }
    if (number == 108) {
        struct task *task = &task_pool[current_task_slot];
        struct virtqueue_create_request request;
        if (vm64_user_access(task->page_dir, arg0, sizeof(request), 1) ||
            vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)))
            return (u64)-1;
        struct kernel_object *device = handle_get(
            task, request.device_handle, KRIGHT_CONTROL, KOBJECT_VIRTIO_DEVICE);
        struct kernel_object *queue = device ? virtqueue_create(
            device, request.queue_index, request.queue_size) : 0;
        struct virtqueue_info *info = virtqueue_get(queue);
        if (!queue || !info) {
            if (queue) object_release(queue);
            return (u64)-1;
        }
        u32 handle = handle_open(
            task, queue, KRIGHT_READ | KRIGHT_MAP |
            KRIGHT_CONTROL | KRIGHT_TRANSFER);
        request.queue_handle = handle;
        request.queue_size = info->queue_size;
        request.descriptor_offset = info->descriptor_offset;
        request.available_offset = info->available_offset;
        request.used_offset = info->used_offset;
        request.total_bytes = info->total_bytes;
        object_release(queue);
        if (!handle || vm64_copy_to(task->page_dir, arg0, &request,
                                    sizeof(request))) {
            if (handle) handle_close(task, handle);
            return (u64)-1;
        }
        return 0;
    }
    if (number == 109) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *queue = handle_get(
            task, (u32)arg0, KRIGHT_MAP, KOBJECT_VIRTQUEUE);
        struct kernel_object *dma = virtqueue_dma_object(queue);
        const struct dma_resource *memory = dma_resource_get(dma);
        if (!queue || !memory) return (u64)-1;
        return (u64)(i64)vm64_map_object(
            task_contexts[current_task_slot].vm_space, arg1, dma,
            memory->physical, (usize_t)memory->pages * 4096,
            0, VM64_CACHE_WB);
    }
    if (number == 110) {
        struct kernel_object *queue = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_VIRTQUEUE);
        return queue ? (u64)(i64)virtqueue_notify(queue) : (u64)-1;
    }
    if (number == 111) {
        struct kernel_object *device = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_VIRTIO_DEVICE);
        return device ? (u64)(i64)virtio_pci_set_driver_ok(device) : (u64)-1;
    }
    if (number == 112) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *device = handle_get(
            task, (u32)arg0, KRIGHT_READ, KOBJECT_VIRTIO_DEVICE);
        struct virtio_config_request request;
        if (!device || vm64_user_access(task->page_dir, arg1,
                                        sizeof(request), 1) ||
            vm64_copy_from(task->page_dir, &request, arg1,
                           sizeof(request)) || request.reserved ||
            !request.length || request.length > VIRTIO_CONFIG_DATA_MAX ||
            virtio_pci_read_config(device, request.offset, request.data,
                                   request.length, &request.generation))
            return (u64)-1;
        return (u64)(i64)vm64_copy_to(task->page_dir, arg1, &request,
                                      sizeof(request));
    }
    if (number == 113) {
        struct task *task = &task_pool[current_task_slot];
        struct virtqueue_chain_request request;
        if (vm64_user_access(task->page_dir, arg0, sizeof(request), 1) ||
            vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
            request.reserved)
            return (u64)-1;
        struct kernel_object *queue = handle_get(
            task, request.queue_handle, KRIGHT_CONTROL, KOBJECT_VIRTQUEUE);
        if (!queue || virtqueue_chain_allocate(
                queue, request.descriptor_count, &request.token))
            return (u64)-1;
        if (vm64_copy_to(task->page_dir, arg0, &request, sizeof(request))) {
            virtqueue_chain_release(queue, request.token);
            return (u64)-1;
        }
        return 0;
    }
    if (number == 114) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct virtqueue_packet_request request;
        if (!domain || vm64_copy_from(task->page_dir, &request, arg0,
                                     sizeof(request)) || request.writable > 1)
            return (u64)-1;
        struct kernel_object *queue = handle_get(
            task, request.queue_handle, KRIGHT_CONTROL, KOBJECT_VIRTQUEUE);
        struct kernel_object *interface = handle_get(
            task, request.interface_handle, KRIGHT_CONTROL,
            KOBJECT_NET_INTERFACE);
        struct kernel_object *pool = interface ?
            net_interface_pool(interface, domain) : 0;
        return queue && pool ? (u64)(i64)virtqueue_descriptor_set_packet(
            queue, request.token, request.ordinal, pool, request.buffer_id,
            request.offset, request.length, request.writable) : (u64)-1;
    }
    if (number == 115 || number == 117) {
        struct task *task = &task_pool[current_task_slot];
        struct virtqueue_chain_request request;
        if (vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
            request.reserved)
            return (u64)-1;
        struct kernel_object *queue = handle_get(
            task, request.queue_handle, KRIGHT_CONTROL, KOBJECT_VIRTQUEUE);
        if (!queue) return (u64)-1;
        return number == 115 ? (u64)(i64)virtqueue_publish(
            queue, request.token) : (u64)(i64)virtqueue_chain_release(
            queue, request.token);
    }
    if (number == 116) {
        struct task *task = &task_pool[current_task_slot];
        struct virtqueue_completion_result result;
        if (vm64_user_access(task->page_dir, arg0, sizeof(result), 1) ||
            vm64_copy_from(task->page_dir, &result, arg0, sizeof(result)))
            return (u64)-1;
        struct kernel_object *queue = handle_get(
            task, result.queue_handle, KRIGHT_CONTROL, KOBJECT_VIRTQUEUE);
        struct virtqueue_completion completion;
        int collected = queue ? virtqueue_collect(queue, &completion) : -1;
        if (collected <= 0) return (u64)(i64)collected;
        result.token = completion.token;
        result.length = completion.length;
        return vm64_copy_to(task->page_dir, arg0, &result, sizeof(result)) ?
            (u64)-1 : 1;
    }
    if (number == 118) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        return interface && domain ?
            net_interface_driver_acquire_rx(interface, domain) : 0;
    }
    if (number == 119) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        struct net_interface_buffer_request request;
        if (!interface || !domain || vm64_copy_from(
                task->page_dir, &request, arg1, sizeof(request)))
            return (u64)-1;
        return (u64)(i64)net_interface_driver_receive(
            interface, domain, request.buffer_id, request.offset,
            request.length, timer_ticks);
    }
    if (number == 120) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        struct net_packet_descriptor descriptor;
        if (!interface || !domain || vm64_user_access(
                task->page_dir, arg1, sizeof(descriptor), 1) ||
            net_interface_driver_dequeue_tx(interface, domain, &descriptor))
            return (u64)-1;
        return (u64)(i64)vm64_copy_to(task->page_dir, arg1, &descriptor,
                                      sizeof(descriptor));
    }
    if (number == 121 || number == 122) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        if (!interface || !domain) return (u64)-1;
        return number == 121 ?
            (u64)(i64)net_interface_driver_complete_tx(
                interface, domain, arg1) :
            (u64)(i64)net_interface_driver_release_rx(
                interface, domain, arg1);
    }
    if (number == 170) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        u32 count = (u32)arg1;
        if (!interface || !domain || !count ||
            count > NET_INTERFACE_BATCH_MAX ||
            vm64_user_access(task->page_dir, arg2, (usize_t)count * 8, 1))
            return (u64)-1;
        u64 buffer_ids[NET_INTERFACE_BATCH_MAX];
        u32 acquired = net_interface_driver_acquire_rx_batch(
            interface, domain, count, buffer_ids);
        if (acquired && vm64_copy_to(task->page_dir, arg2, buffer_ids,
                (usize_t)acquired * 8))
            return (u64)-1;
        return (u64)acquired;
    }
    if (number == 171) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        u32 count = (u32)arg1;
        if (!interface || !domain || !count ||
            count > NET_INTERFACE_BATCH_MAX ||
            vm64_user_access(task->page_dir, arg2,
                (usize_t)count * sizeof(struct net_interface_buffer_request),
                0))
            return (u64)-1;
        struct net_interface_buffer_request requests[NET_INTERFACE_BATCH_MAX];
        if (vm64_copy_from(task->page_dir, requests, arg2,
                (usize_t)count * sizeof(struct net_interface_buffer_request)))
            return (u64)-1;
        return (u64)net_interface_driver_receive_batch(
            interface, domain, requests, count, timer_ticks);
    }
    if (number == 172) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        u32 count = (u32)arg1;
        if (!interface || !domain || !count ||
            count > NET_INTERFACE_BATCH_MAX ||
            vm64_user_access(task->page_dir, arg2,
                (usize_t)count * sizeof(struct net_packet_descriptor), 1))
            return (u64)-1;
        struct net_packet_descriptor descriptors[NET_INTERFACE_BATCH_MAX];
        u32 dequeued = net_interface_driver_dequeue_tx_batch(
            interface, domain, descriptors, count);
        if (dequeued && vm64_copy_to(task->page_dir, arg2, descriptors,
                (usize_t)dequeued * sizeof(struct net_packet_descriptor)))
            return (u64)-1;
        return (u64)dequeued;
    }
    if (number == 173) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        u32 count = (u32)arg1;
        if (!interface || !domain || !count ||
            count > NET_INTERFACE_BATCH_MAX ||
            vm64_user_access(task->page_dir, arg2, (usize_t)count * 8, 0))
            return (u64)-1;
        u64 buffer_ids[NET_INTERFACE_BATCH_MAX];
        if (vm64_copy_from(task->page_dir, buffer_ids, arg2,
                (usize_t)count * 8))
            return (u64)-1;
        return (u64)net_interface_driver_complete_tx_batch(
            interface, domain, buffer_ids, count);
    }
    if (number == 123) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        struct net_interface_ipv4_request request;
        if (!interface || !domain || vm64_copy_from(
                task->page_dir, &request, arg1, sizeof(request)) ||
            request.reserved)
            return (u64)-1;
        return (u64)(i64)net_interface_configure_ipv4(
            interface, domain, request.address, request.netmask,
            request.gateway);
    }
    if (number == 124) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        struct net_interface_echo_request request;
        if (!interface || !domain || vm64_copy_from(
                task->page_dir, &request, arg1, sizeof(request)))
            return (u64)-1;
        return (u64)(i64)net_interface_send_echo(
            interface, domain, request.destination,
            request.identifier, request.sequence);
    }
    if (number == 125) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_READ, KOBJECT_NET_INTERFACE);
        return interface && domain ?
            net_interface_echo_replies(interface, domain) : 0;
    }
    if (number == 126 || number == 127) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        if (!interface || !domain) return (u64)-1;
        return number == 126 ?
            (u64)(i64)net_interface_send_udp_probe(
                interface, domain, (u32)arg1) :
            (u64)(i64)net_interface_poll_udp_probe(interface, domain);
    }
    if (number == 128) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *queue = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_VIRTQUEUE);
        struct kernel_object *irq = handle_get(
            task, (u32)arg1, KRIGHT_CONTROL, KOBJECT_IRQ);
        return queue && irq ?
            (u64)(i64)virtqueue_set_msix(queue, irq) : (u64)-1;
    }
    if (number == 129) {
        struct task *task = &task_pool[current_task_slot];
        struct virtqueue_completion_batch batch;
        if (vm64_user_access(task->page_dir, arg0, sizeof(batch), 1) ||
            vm64_copy_from(task->page_dir, &batch, arg0, sizeof(batch)) ||
            batch.reserved || !batch.maximum ||
            batch.maximum > VIRTQUEUE_COMPLETION_BATCH_MAX)
            return (u64)-1;
        struct kernel_object *queue = handle_get(
            task, batch.queue_handle, KRIGHT_CONTROL, KOBJECT_VIRTQUEUE);
        struct virtqueue_completion completions[
            VIRTQUEUE_COMPLETION_BATCH_MAX];
        int count = queue ? virtqueue_collect_batch(
            queue, completions, batch.maximum) : -1;
        if (count < 0) return (u64)-1;
        batch.count = (u32)count;
        for (u32 index = 0; index < batch.count; index++) {
            batch.items[index].token = completions[index].token;
            batch.items[index].length = completions[index].length;
            batch.items[index].reserved = 0;
        }
        return (u64)(i64)vm64_copy_to(
            task->page_dir, arg0, &batch, sizeof(batch));
    }
    if (number == 130 || number == 131 || number == 133 || number == 134) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0,
            number == 134 ? KRIGHT_READ : KRIGHT_CONTROL,
            KOBJECT_NET_INTERFACE);
        if (!interface || !domain) return (u64)-1;
        if (number == 130)
            return (u64)(i64)net_interface_ipv6_start(interface, domain);
        if (number == 131)
            return (u64)(i64)net_interface_ipv6_complete_dad(
                interface, domain);
        if (number == 133)
            return (u64)(i64)net_interface_ipv6_send_echo(interface, domain);
        return net_interface_ipv6_echo_replies(interface, domain);
    }
    if (number == 132) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_READ, KOBJECT_NET_INTERFACE);
        struct net_interface_ipv6_info info;
        if (!interface || !domain || vm64_user_access(
                task->page_dir, arg1, sizeof(info), 1) ||
            net_interface_ipv6_info(
                interface, domain, &info.state, &info.duplicate,
                &info.deprecated, info.link_local, info.global,
                info.router))
            return (u64)-1;
        info.reserved = 0;
        info.echo_replies = net_interface_ipv6_echo_replies(
            interface, domain);
        return (u64)(i64)vm64_copy_to(
            task->page_dir, arg1, &info, sizeof(info));
    }
    if (number == 135 || number == 136) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        if (!interface || !domain) return (u64)-1;
        return number == 135 ?
            (u64)(i64)net_interface_udpv6_start_probe(interface, domain) :
            (u64)(i64)net_interface_udpv6_poll_probe(interface, domain);
    }
    if (number == 137) {
        struct kernel_object *socket = socket_create_ipv6();
        if (!socket) return (u64)-1;
        u32 handle = handle_open(
            &task_pool[current_task_slot], socket,
            KRIGHT_READ | KRIGHT_WRITE | KRIGHT_WAIT | KRIGHT_CONTROL);
        object_release(socket);
        return handle ? handle : (u64)-1;
    }
    if (number == 138) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *socket = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_SOCKET);
        struct socket_ipv6_bind_request request;
        if (!socket || vm64_copy_from(task->page_dir, &request, arg1,
                                     sizeof(request)) || request.reserved)
            return (u64)-1;
        return (u64)(i64)socket_bind_ipv6(
            socket, request.address, request.port);
    }
    if (number == 139) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *socket = handle_get(
            task, (u32)arg0, KRIGHT_WRITE, KOBJECT_SOCKET);
        struct socket_ipv6_send_request request;
        if (!socket || vm64_copy_from(task->page_dir, &request, arg1,
                                     sizeof(request)) ||
            request.length > SOCKET_PAYLOAD_MAX)
            return (u64)-1;
        return (u64)(i64)socket_send_to_ipv6(
            socket, request.destination_address,
            request.destination_port, request.payload, request.length);
    }
    if (number == 144) {
        struct kernel_object *socket = socket_create_stream();
        if (!socket) return (u64)-1;
        u32 handle = handle_open(
            &task_pool[current_task_slot], socket,
            KRIGHT_READ | KRIGHT_WRITE | KRIGHT_WAIT | KRIGHT_CONTROL);
        object_release(socket);
        return handle ? handle : (u64)-1;
    }
    if (number == 145) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *socket = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_SOCKET);
        struct socket_stream_connect_request request;
        if (!socket || !domain || vm64_copy_from(
                task->page_dir, &request, arg1, sizeof(request)) ||
            request.reserved)
            return (u64)-1;
        struct kernel_object *interface = handle_get(
            task, request.interface_handle, KRIGHT_CONTROL,
            KOBJECT_NET_INTERFACE);
        if (!interface || !net_interface_pool(interface, domain))
            return (u64)-1;
        return (u64)(i64)socket_stream_connect(
            socket, interface, request.destination_address,
            request.destination_port);
    }
    if (number == 146 || number == 147) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *socket = handle_get(
            task, (u32)arg0, number == 146 ? KRIGHT_WRITE : KRIGHT_READ,
            KOBJECT_SOCKET);
        struct socket_stream_data data;
        if (!socket || vm64_user_access(task->page_dir, arg1,
                                        sizeof(data), 1) ||
            vm64_copy_from(task->page_dir, &data, arg1, sizeof(data)) ||
            data.reserved || data.length > SOCKET_STREAM_PAYLOAD_MAX)
            return (u64)-1;
        if (number == 146)
            return (u64)(i64)socket_stream_send(
                socket, data.data, data.length);
        u32 received = 0;
        if (socket_stream_receive(socket, data.data,
                                  SOCKET_STREAM_PAYLOAD_MAX, &received))
            return (u64)-1;
        data.length = received;
        return (u64)(i64)vm64_copy_to(
            task->page_dir, arg1, &data, sizeof(data));
    }
    if (number == 148) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *socket = handle_get(
            task, (u32)arg0, KRIGHT_READ, KOBJECT_SOCKET);
        struct socket_stream_state_result result;
        if (!socket || vm64_user_access(task->page_dir, arg1,
                                        sizeof(result), 1) ||
            socket_stream_state(socket, &result.state,
                                &result.readiness,
                                &result.error, &result.eof))
            return (u64)-1;
        return (u64)(i64)vm64_copy_to(
            task->page_dir, arg1, &result, sizeof(result));
    }
    if (number == 149) {
        struct kernel_object *socket = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_SOCKET);
        return socket ? (u64)(i64)socket_stream_shutdown(socket) : (u64)-1;
    }
    if (number == 154) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *socket = handle_get(
            task, (u32)arg0, KRIGHT_READ, KOBJECT_SOCKET);
        struct socket_stream_error_result result;
        if (!socket || vm64_user_access(task->page_dir, arg1,
                                        sizeof(result), 1) ||
            socket_stream_take_error(socket, &result.error))
            return (u64)-1;
        result.reserved = 0;
        return (u64)(i64)vm64_copy_to(
            task->page_dir, arg1, &result, sizeof(result));
    }
    if (number == 152 || number == 153) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        if (!interface || !domain) return (u64)-1;
        return number == 152 ?
            (u64)(i64)net_interface_tcpv6_probe_start(
                interface, domain, (u16)arg1) :
            (u64)(i64)net_interface_tcpv6_probe_poll(interface, domain);
    }
    if (number == 150 || number == 167) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *socket = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_SOCKET);
        struct socket_stream_listen_request request;
        if (!socket || !domain || vm64_copy_from(
                task->page_dir, &request, arg1, sizeof(request)))
            return (u64)-1;
        struct kernel_object *interface = handle_get(
            task, request.interface_handle, KRIGHT_CONTROL,
            KOBJECT_NET_INTERFACE);
        if (!interface || !net_interface_pool(interface, domain))
            return (u64)-1;
        return number == 150 ?
            (u64)(i64)socket_stream_listen(
                socket, interface, request.local_port, request.backlog) :
            (u64)(i64)socket_stream_listen_ipv6(
                socket, interface, request.local_port, request.backlog);
    }
    if (number == 151) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *listener = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_SOCKET);
        struct kernel_object *accepted = listener ?
            socket_stream_accept(listener) : 0;
        if (!accepted) return (u64)-1;
        u32 handle = handle_open(
            task, accepted, KRIGHT_READ | KRIGHT_WRITE |
            KRIGHT_WAIT | KRIGHT_CONTROL);
        object_release(accepted);
        return handle ? handle : (u64)-1;
    }
    if (number == 142 || number == 143) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        if (!interface || !domain) return (u64)-1;
        return number == 142 ?
            (u64)(i64)net_interface_tcp_probe_start(
                interface, domain, (u32)arg1, (u16)arg2) :
            (u64)(i64)net_interface_tcp_probe_poll(interface, domain);
    }
    if (number == 141) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct kernel_object *interface = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_NET_INTERFACE);
        if (!interface || !domain ||
            net_interface_pool(interface, domain) == 0)
            return (u64)-1;
        net_interface_tick(interface, timer_ticks);
        return 0;
    }
    if (number == 140) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *socket = handle_get(
            task, (u32)arg0, KRIGHT_READ, KOBJECT_SOCKET);
        struct udpv6_datagram datagram;
        if (!socket || vm64_user_access(task->page_dir, arg1,
                                        sizeof(struct socket_ipv6_receive_result), 1) ||
            socket_receive_from_ipv6(socket, &datagram))
            return (u64)-1;
        struct socket_ipv6_receive_result result;
        for (u32 index = 0; index < 16; index++) {
            result.source_address[index] = datagram.source[index];
            result.destination_address[index] = datagram.destination[index];
        }
        result.source_port = datagram.source_port;
        result.destination_port = datagram.destination_port;
        result.length = datagram.length;
        result.reserved = 0;
        for (u32 index = 0; index < SOCKET_PAYLOAD_MAX; index++)
            result.payload[index] = index < datagram.length ?
                datagram.payload[index] : 0;
        return (u64)(i64)vm64_copy_to(
            task->page_dir, arg1, &result, sizeof(result));
    }
    if (number == 169) {
        return (u64)(i64)driver_domain_stop_ack(
            task_pool[current_task_slot].id);
    }
    if (number == 168) {
        struct task *task = &task_pool[current_task_slot];
        struct driver_domain *domain = driver_domain_for_pid(task->id);
        struct firmware_open_request request;
        if (!domain || vm64_copy_from(task->page_dir, &request, arg0,
                                     sizeof(request)) ||
            request.file_handle || request.size || request.reserved0 ||
            request.reserved1 || request.name[FIRMWARE_NAME_MAX - 1] ||
            !driver_domain_firmware_allowed(domain, request.name))
            return (u64)-1;
        u32 size = 0;
        struct kernel_object *file = firmware_open(request.name, &size);
        if (!file) return (u64)-1;
        u32 handle = handle_open(task, file, KRIGHT_READ);
        object_release(file);
        if (!handle) return (u64)-1;
        request.file_handle = handle;
        request.size = size;
        if (vm64_copy_to(task->page_dir, arg0, &request, sizeof(request))) {
            handle_close(task, handle);
            return (u64)-1;
        }
        return 0;
    }
    if (number == 155) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *root = vfs_root();
        if (!root) return (u64)-1;
        u32 rights = KRIGHT_READ | KRIGHT_TRANSFER;
        if (task->capabilities & CAP_VFS_ADMIN) rights |= KRIGHT_CONTROL;
        u32 handle = handle_open(task, root, rights);
        object_release(root);
        return handle ? handle : (u64)-1;
    }
    if (number == 156 || number == 157 || number == 163) {
        struct task *task = &task_pool[current_task_slot];
        struct vfs_name_request request;
        if (vm64_user_access(task->page_dir, arg0, sizeof(request),
                             number != 163) ||
            vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
            request.reserved || request.name[VFS_NAME_MAX - 1])
            return (u64)-1;
        u32 rights = number == 157 ? KRIGHT_READ : KRIGHT_CONTROL;
        struct kernel_object *directory = handle_get(
            task, request.directory_handle, rights, KOBJECT_DIRECTORY);
        if (!directory) return (u64)-1;
        if (number == 163) {
            if (!(task->capabilities & CAP_VFS_ADMIN)) return (u64)-1;
            return (u64)(i64)vfs_unlink(directory, request.name);
        }
        struct kernel_object *node;
        if (number == 156) {
            if (!(task->capabilities & CAP_VFS_ADMIN)) return (u64)-1;
            node = vfs_create(directory, request.name, request.type);
        } else {
            node = vfs_lookup(directory, request.name);
        }
        if (!node) return (u64)-1;
        struct vfs_node_info node_info;
        if (number == 157 &&
            (vfs_stat(node, &node_info) ||
             (node_info.filesystem == VFS_FILESYSTEM_BOOTFS &&
              !(task->capabilities & CAP_VFS_ADMIN)))) {
            object_release(node);
            return (u64)-1;
        }
        u32 node_rights = KRIGHT_READ | KRIGHT_CONTROL | KRIGHT_TRANSFER;
        if (number == 156 && node->type == KOBJECT_VNODE)
            node_rights |= KRIGHT_WRITE;
        u32 handle = handle_open(task, node, node_rights);
        object_release(node);
        request.node_handle = handle;
        if (!handle || vm64_copy_to(task->page_dir, arg0,
                                    &request, sizeof(request))) {
            if (handle) handle_close(task, handle);
            return (u64)-1;
        }
        return 0;
    }
    if (number == 158) {
        struct task *task = &task_pool[current_task_slot];
        struct vfs_open_request request;
        if (vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
            request.reserved || !request.rights ||
            (request.rights & ~(KRIGHT_READ | KRIGHT_WRITE)))
            return (u64)-1;
        struct kernel_object *node = handle_get(
            task, request.node_handle, request.rights, KOBJECT_VNODE);
        struct kernel_object *file = node ? vfs_open(node) : 0;
        if (!file) return (u64)-1;
        u32 handle = handle_open(
            task, file, request.rights | KRIGHT_CONTROL | KRIGHT_TRANSFER);
        object_release(file);
        request.file_handle = handle;
        if (!handle || vm64_copy_to(task->page_dir, arg0,
                                    &request, sizeof(request))) {
            if (handle) handle_close(task, handle);
            return (u64)-1;
        }
        return 0;
    }
    if (number == 159 || number == 160) {
        struct task *task = &task_pool[current_task_slot];
        struct vfs_io_request request;
        if (vm64_user_access(task->page_dir, arg0, sizeof(request), 1) ||
            vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
            request.length > VFS_IO_MAX)
            return (u64)-1;
        struct kernel_object *file = handle_get(
            task, request.file_handle,
            number == 159 ? KRIGHT_READ : KRIGHT_WRITE, KOBJECT_FILE);
        if (!file) return (u64)-1;
        int result = number == 159 ?
            vfs_read(file, request.offset, request.data,
                     request.length, &request.transferred) :
            vfs_write(file, request.offset, request.data,
                      request.length, &request.transferred);
        if (result || vm64_copy_to(task->page_dir, arg0,
                                   &request, sizeof(request)))
            return (u64)-1;
        return 0;
    }
    if (number == 161) {
        struct task *task = &task_pool[current_task_slot];
        struct vfs_truncate_request request;
        if (vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)))
            return (u64)-1;
        struct kernel_object *file = handle_get(
            task, request.file_handle, KRIGHT_WRITE, KOBJECT_FILE);
        return file ? (u64)(i64)vfs_truncate(file, request.size) : (u64)-1;
    }
    if (number == 162) {
        struct task *task = &task_pool[current_task_slot];
        struct vfs_stat_request request;
        if (vm64_user_access(task->page_dir, arg0, sizeof(request), 1) ||
            vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
            request.reserved)
            return (u64)-1;
        struct kernel_object *object = handle_get(
            task, request.handle, KRIGHT_READ, KOBJECT_NONE);
        if (!object || vfs_stat(object, &request.info) ||
            vm64_copy_to(task->page_dir, arg0, &request, sizeof(request)))
            return (u64)-1;
        return 0;
    }
    if (number == 164 || number == 165 || number == 166) {
        struct task *task = &task_pool[current_task_slot];
        struct vfs_path_request request;
        if (vm64_user_access(task->page_dir, arg0, sizeof(request),
                             number != 166) ||
            vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
            request.reserved || request.path[VFS_PATH_MAX - 1])
            return (u64)-1;
        struct kernel_object *start = 0;
        if (request.path[0] != '/') {
            start = handle_get(
                task, request.start_handle,
                number == 164 ? KRIGHT_READ : KRIGHT_CONTROL,
                KOBJECT_DIRECTORY);
            if (!start) return (u64)-1;
        }
        if (number == 166) {
            if (!(task->capabilities & CAP_VFS_ADMIN)) return (u64)-1;
            return (u64)(i64)vfs_unlink_path(start, request.path);
        }
        struct kernel_object *node;
        if (number == 165) {
            if (!(task->capabilities & CAP_VFS_ADMIN)) return (u64)-1;
            node = vfs_create_path(start, request.path, request.type);
        } else {
            node = vfs_resolve(start, request.path);
        }
        if (!node) return (u64)-1;
        struct vfs_node_info node_info;
        if (number == 164 &&
            (vfs_stat(node, &node_info) ||
             (node_info.filesystem == VFS_FILESYSTEM_BOOTFS &&
              !(task->capabilities & CAP_VFS_ADMIN)))) {
            object_release(node);
            return (u64)-1;
        }
        u32 rights = KRIGHT_READ | KRIGHT_CONTROL | KRIGHT_TRANSFER;
        if (number == 165 && node->type == KOBJECT_VNODE)
            rights |= KRIGHT_WRITE;
        u32 handle = handle_open(task, node, rights);
        object_release(node);
        request.node_handle = handle;
        if (!handle || vm64_copy_to(task->page_dir, arg0,
                                    &request, sizeof(request))) {
            if (handle) handle_close(task, handle);
            return (u64)-1;
        }
        return 0;
    }
    if (number == 60) {
        struct driver_bootstrap_info bootstrap;
        int result = driver_domain_bootstrap(
            task_pool[current_task_slot].id, &bootstrap);
        if (result) return (u64)-1;
        return (u64)(i64)vm64_copy_to(
            task_pool[current_task_slot].page_dir, arg0,
            &bootstrap, sizeof(bootstrap));
    }
    if (number == 174) return timer_ticks;
    if (number >= 175 && number <= 178) {
        struct task *task = &task_pool[current_task_slot];
        u32 rights = number == 178 ? KRIGHT_CONTROL : KRIGHT_READ;
        struct kernel_object *pci = handle_get(
            task, (u32)arg0, rights, KOBJECT_PCI);
        if (!pci || arg1 > 255) return (u64)-1;
        if (number == 175) {
            u8 value;
            return pci64_config_read8(pci, (u16)arg1, &value) ? (u64)-1 : value;
        }
        if (number == 176) {
            u16 value;
            return pci64_config_read16(pci, (u16)arg1, &value) ? (u64)-1 : value;
        }
        if (number == 177) {
            u32 value;
            return pci64_config_read32(pci, (u16)arg1, &value) ? (u64)-1 : value;
        }
        if (arg1 != 4) return (u64)-1;
        u16 current;
        u16 value = (u16)arg2;
        if (pci64_config_read16(pci, 4, &current) ||
            ((current ^ value) & (u16)~7u))
            return (u64)-1;
        return (u64)(i64)pci64_set_command(
            pci, value & (u16)~current, current & (u16)~value);
    }
    if (number == 179) {
        struct task *task = &task_pool[current_task_slot];
        if (!(task->capabilities & CAP_RESOURCE_ADMIN))
            return (u64)(i64)EPERM;
        struct kernel_object *object = block_create((u32)arg0, (u32)arg1);
        if (!object) return (u64)-1;
        u32 rights = KRIGHT_READ | KRIGHT_CONTROL | KRIGHT_TRANSFER |
                     KRIGHT_WAIT;
        if (!((u32)arg1 & BLOCK_FLAG_READ_ONLY)) rights |= KRIGHT_WRITE;
        u32 handle = handle_open(task, object, rights);
        object_release(object);
        return handle ? handle : (u64)-1;
    }
    if (number == 180) {
        struct task *task = &task_pool[current_task_slot];
        struct kernel_object *object = handle_get(
            task, (u32)arg0, KRIGHT_READ, KOBJECT_BLOCK);
        struct block_info info;
        if (!object || block_info(object, &info)) return (u64)-1;
        return (u64)(i64)vm64_copy_to(task->page_dir, arg1, &info,
                                      sizeof(info));
    }
    if (number == 181) {
        struct task *task = &task_pool[current_task_slot];
        struct block_io_request request;
        if (vm64_user_access(task->page_dir, arg0, sizeof(request), 1) ||
            vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
            !request.sectors || request.sectors > BLOCK_IO_SECTORS_MAX)
            return (u64)-1;
        u32 need = request.op == BLOCK_OP_WRITE ? KRIGHT_WRITE : KRIGHT_READ;
        struct kernel_object *object = handle_get(
            task, request.device_handle, need, KOBJECT_BLOCK);
        u64 id = 0;
        if (!object ||
            block_submit(object, request.op, request.lba, request.sectors,
                         request.data, request.sectors * BLOCK_SECTOR_SIZE,
                         &id))
            return (u64)-1;
        request.request_id = id;
        request.status = 0;
        request.transferred = request.sectors * BLOCK_SECTOR_SIZE;
        return (u64)(i64)vm64_copy_to(task->page_dir, arg0, &request,
                                      sizeof(request));
    }
    if (number == 182) {
        struct task *task = &task_pool[current_task_slot];
        struct block_io_request request;
        if (vm64_user_access(task->page_dir, arg0, sizeof(request), 1) ||
            vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)))
            return (u64)-1;
        struct kernel_object *object = handle_get(
            task, request.device_handle, KRIGHT_READ, KOBJECT_BLOCK);
        i32 status = 0;
        u32 transferred = 0;
        if (!object ||
            block_collect(object, request.request_id, &status, &transferred,
                          request.data, BLOCK_SECTOR_SIZE))
            return (u64)-1;
        request.status = status;
        request.transferred = transferred;
        return (u64)(i64)vm64_copy_to(task->page_dir, arg0, &request,
                                      sizeof(request));
    }
    if (number == 183) {
        struct kernel_object *object = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_BLOCK);
        return object ? (u64)(i64)block_revoke(object) : (u64)-1;
    }
    if (number == 184) {
        struct kernel_object *object = handle_get(
            &task_pool[current_task_slot], (u32)arg0,
            KRIGHT_CONTROL, KOBJECT_BLOCK);
        return object ? (u64)(i64)block_service(object) : (u64)-1;
    }
    if (number == 185) {
        struct task *task = &task_pool[current_task_slot];
        if (!(task->capabilities & CAP_RESOURCE_ADMIN))
            return (u64)(i64)EPERM;
        struct kernel_object *pci = handle_get(
            task, (u32)arg0, KRIGHT_CONTROL, KOBJECT_PCI);
        struct kernel_object *object = pci ? virtio_blk_open(pci) : 0;
        if (!object) return (u64)-1;
        u32 rights = KRIGHT_READ | KRIGHT_CONTROL | KRIGHT_TRANSFER |
                     KRIGHT_WAIT;
        struct block_info info;
        if (!block_info(object, &info) &&
            !(info.flags & BLOCK_FLAG_READ_ONLY))
            rights |= KRIGHT_WRITE;
        u32 handle = handle_open(task, object, rights);
        object_release(object);
        return handle ? handle : (u64)-1;
    }
    if (number >= POSIX_SYSCALL_OPEN && number <= POSIX_SYSCALL_TRUNCATE) {
        struct task *task = &task_pool[current_task_slot];
        if (!posix_profile_admitted(task)) return (u64)(i64)POSIX_VFS_EACCES;
        if (number == POSIX_SYSCALL_OPEN) {
            struct posix_open_request request;
            if (vm64_user_access(task->page_dir, arg0, sizeof(request), 0) ||
                vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
                request.path[VFS_PATH_MAX - 1])
                return (u64)(i64)POSIX_VFS_EINVAL;
            return (u64)(i64)posix_vfs_open(task, request.path, request.flags,
                                             request.mode);
        }
        if (number == POSIX_SYSCALL_CLOSE || number == POSIX_SYSCALL_DUP) {
            struct posix_fd_request request;
            if (vm64_user_access(task->page_dir, arg0, sizeof(request), 0) ||
                vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
                request.reserved)
                return (u64)(i64)POSIX_VFS_EINVAL;
            int error = posix_fd_error(task, request.descriptor, 0);
            if (error) return (u64)(i64)error;
            int result = number == POSIX_SYSCALL_CLOSE ?
                posix_fd_close(task, request.descriptor) :
                posix_fd_dup(task, request.descriptor);
            if (result >= 0) return (u64)(i64)result;
            return (u64)(i64)(number == POSIX_SYSCALL_DUP ?
                POSIX_VFS_EMFILE : POSIX_VFS_EBADF);
        }
        if (number == POSIX_SYSCALL_READ || number == POSIX_SYSCALL_WRITE) {
            struct posix_io_request request;
            int writable = number == POSIX_SYSCALL_READ;
            if (vm64_user_access(task->page_dir, arg0, sizeof(request), writable) ||
                vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
                request.length > POSIX_IO_MAX)
                return (u64)(i64)POSIX_VFS_EINVAL;
            u32 access = number == POSIX_SYSCALL_READ ? POSIX_FD_ACCESS_READ :
                POSIX_FD_ACCESS_WRITE;
            int error = posix_fd_error(task, request.descriptor, access);
            if (error) return (u64)(i64)error;
            if (number == POSIX_SYSCALL_WRITE &&
                !posix_profile_vfs_authorized(task))
                return (u64)(i64)POSIX_VFS_EACCES;
            int result = number == POSIX_SYSCALL_READ ?
                posix_fd_read(task, request.descriptor, request.data,
                              request.length, &request.transferred) :
                posix_fd_write(task, request.descriptor, request.data,
                               request.length, &request.transferred);
            if (result) return (u64)(i64)POSIX_VFS_EIO;
            if (number == POSIX_SYSCALL_READ &&
                vm64_copy_to(task->page_dir, arg0, &request, sizeof(request)))
                return (u64)(i64)POSIX_VFS_EIO;
            return (u64)(i64)request.transferred;
        }
        if (number == POSIX_SYSCALL_LSEEK) {
            struct posix_seek_request request;
            if (vm64_user_access(task->page_dir, arg0, sizeof(request), 1) ||
                vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
                request.reserved || request.whence > POSIX_SEEK_END)
                return (u64)(i64)POSIX_VFS_EINVAL;
            int error = posix_fd_error(task, request.descriptor, 0);
            if (error) return (u64)(i64)error;
            if (posix_fd_seek(task, request.descriptor, request.offset,
                              request.whence, &request.position))
                return (u64)(i64)POSIX_VFS_EINVAL;
            if (vm64_copy_to(task->page_dir, arg0, &request, sizeof(request)))
                return (u64)(i64)POSIX_VFS_EIO;
            return (u64)(i64)request.position;
        }
        if (number == POSIX_SYSCALL_DUP2) {
            struct posix_dup2_request request;
            if (vm64_user_access(task->page_dir, arg0, sizeof(request), 0) ||
                vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)))
                return (u64)(i64)POSIX_VFS_EINVAL;
            int error = posix_fd_error(task, request.descriptor, 0);
            if (error) return (u64)(i64)error;
            if (request.replacement < 0 || request.replacement >= POSIX_FD_MAX)
                return (u64)(i64)POSIX_VFS_EINVAL;
            int result = posix_fd_dup2(task, request.descriptor,
                                       request.replacement);
            return (u64)(i64)(result < 0 ? POSIX_VFS_EBADF : result);
        }
        if (number == POSIX_SYSCALL_FCNTL) {
            struct posix_fcntl_request request;
            if (vm64_user_access(task->page_dir, arg0, sizeof(request), 1) ||
                vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
                request.reserved || (request.command != POSIX_FCNTL_GETFD &&
                                     request.command != POSIX_FCNTL_SETFD) ||
                (request.command == POSIX_FCNTL_GETFD && request.argument) ||
                (request.command == POSIX_FCNTL_SETFD && request.argument > 1))
                return (u64)(i64)POSIX_VFS_EINVAL;
            int error = posix_fd_error(task, request.descriptor, 0);
            if (error) return (u64)(i64)error;
            if (request.command == POSIX_FCNTL_GETFD) {
                u32 enabled = 0;
                if (posix_fd_get_cloexec(task, request.descriptor, &enabled))
                    return (u64)(i64)POSIX_VFS_EBADF;
                return enabled ? POSIX_FD_CLOEXEC_VALUE : 0;
            }
            return (u64)(i64)(posix_fd_set_cloexec(task, request.descriptor,
                                                    request.argument) ?
                POSIX_VFS_EBADF : 0);
        }
        if (number == POSIX_SYSCALL_STAT) {
            struct posix_stat_path_request request;
            struct vfs_node_info info;
            if (vm64_user_access(task->page_dir, arg0, sizeof(request), 1) ||
                vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
                request.path[VFS_PATH_MAX - 1])
                return (u64)(i64)POSIX_VFS_EINVAL;
            int result = posix_vfs_stat_path(task, request.path, &info);
            if (result) return (u64)(i64)result;
            posix_stat_record(&info, &request.stat);
            return vm64_copy_to(task->page_dir, arg0, &request, sizeof(request)) ?
                (u64)(i64)POSIX_VFS_EIO : 0;
        }
        if (number == POSIX_SYSCALL_FSTAT) {
            struct posix_fstat_request request;
            struct vfs_node_info info;
            if (vm64_user_access(task->page_dir, arg0, sizeof(request), 1) ||
                vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
                request.reserved)
                return (u64)(i64)POSIX_VFS_EINVAL;
            int error = posix_fd_error(task, request.descriptor, 0);
            if (error) return (u64)(i64)error;
            if (posix_fd_stat(task, request.descriptor, &info))
                return (u64)(i64)POSIX_VFS_EIO;
            posix_stat_record(&info, &request.stat);
            return vm64_copy_to(task->page_dir, arg0, &request, sizeof(request)) ?
                (u64)(i64)POSIX_VFS_EIO : 0;
        }
        if (number == POSIX_SYSCALL_MKDIR || number == POSIX_SYSCALL_RMDIR ||
            number == POSIX_SYSCALL_UNLINK || number == POSIX_SYSCALL_CHDIR) {
            struct posix_mode_path_request mode_request;
            struct posix_path_request path_request;
            const char *path = 0;
            u32 mode = 0;
            if (number == POSIX_SYSCALL_MKDIR) {
                if (vm64_user_access(task->page_dir, arg0, sizeof(mode_request), 0) ||
                    vm64_copy_from(task->page_dir, &mode_request, arg0,
                                   sizeof(mode_request)) ||
                    mode_request.path[VFS_PATH_MAX - 1])
                    return (u64)(i64)POSIX_VFS_EINVAL;
                path = mode_request.path;
                mode = mode_request.mode;
            } else {
                if (vm64_user_access(task->page_dir, arg0, sizeof(path_request), 0) ||
                    vm64_copy_from(task->page_dir, &path_request, arg0,
                                   sizeof(path_request)) ||
                    path_request.path[VFS_PATH_MAX - 1])
                    return (u64)(i64)POSIX_VFS_EINVAL;
                path = path_request.path;
            }
            int result = number == POSIX_SYSCALL_MKDIR ?
                posix_vfs_mkdir(task, path, mode) :
                number == POSIX_SYSCALL_RMDIR ? posix_vfs_rmdir(task, path) :
                number == POSIX_SYSCALL_UNLINK ? posix_vfs_unlink(task, path) :
                posix_profile_chdir(task, path);
            return (u64)(i64)result;
        }
        if (number == POSIX_SYSCALL_GETCWD) {
            struct posix_getcwd_request request;
            if (vm64_user_access(task->page_dir, arg0, sizeof(request), 1) ||
                vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
                !request.capacity || request.capacity > VFS_PATH_MAX)
                return (u64)(i64)POSIX_VFS_EINVAL;
            int result = posix_profile_getcwd(task, request.path);
            if (result) return (u64)(i64)result;
            u32 length = 0;
            while (length < VFS_PATH_MAX && request.path[length]) length++;
            if (length == VFS_PATH_MAX || length + 1 > request.capacity)
                return (u64)(i64)POSIX_VFS_ERANGE;
            request.length = length;
            return vm64_copy_to(task->page_dir, arg0, &request, sizeof(request)) ?
                (u64)(i64)POSIX_VFS_EIO : 0;
        }
        if (number == POSIX_SYSCALL_TRUNCATE) {
            struct posix_truncate_request request;
            if (vm64_user_access(task->page_dir, arg0, sizeof(request), 0) ||
                vm64_copy_from(task->page_dir, &request, arg0, sizeof(request)) ||
                request.path[VFS_PATH_MAX - 1])
                return (u64)(i64)POSIX_VFS_EINVAL;
            return (u64)(i64)posix_vfs_truncate_path(task, request.path,
                                                      request.size);
        }
    }
    if (number == 4) {
        serial64_write("Mich x86_64: syscall/sysret pass\n");
        return 0;
    }
    return (u64)-1;
}
