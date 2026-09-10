#include "syscall.h"
#include "arch_cpu.h"
#include "capability.h"
#include "gfx.h"
#include "ipc.h"
#include "irq.h"
#include "task.h"
#include "scheduler.h"
#include "pmm.h"
#include "proc.h"
#include "serial.h"
#include "service.h"
#include "uaccess.h"
#include "protos.h"

#define SYS_WRITE_CHUNK 64
#define SYS_WRITE_CHUNKS_MAX 16

void syscall_init(void) {
}

int syscall_dispatcher(unsigned int eax, unsigned int ebx, unsigned int ecx, unsigned int edx) {
    switch (eax) {
        case SYS_WRITE: {
            struct task *me = &task_pool[scheduler_current()];
            char buf[SYS_WRITE_CHUNK];
            unsigned int up = ebx;
            int rc = 0;
            for (int guard = 0; guard < SYS_WRITE_CHUNKS_MAX; guard++) {
                irq_state_t state = irq_save();
                int n = ua_copy_str(me->page_dir, buf, up, sizeof(buf));
                if (n >= 0)
                    gfx_write(buf);
                irq_restore(state);
                if (n >= 0)
                    serial_write(buf);
                if (n < 0) { rc = -1; break; }
                if (n < (int)sizeof(buf) - 1 || n == 0) break;
                up += (unsigned int)n;
            }
            return rc;
        }
        case SYS_CLEAR: {
            if (!(task_pool[scheduler_current()].capabilities & CAP_DISPLAY_ADMIN))
                return EPERM;
            irq_state_t state = irq_save();
            gfx_clear();
            irq_restore(state);
            return 0;
        }
        case SYS_SEND:
            return ipc_send(ebx, (const struct message *)ecx);
        case SYS_RECV:
            return ipc_recv((struct message *)ebx);
        case SYS_SEND_NB:
            return ipc_send_nb(ebx, (const struct message *)ecx);
        case SYS_IRQ_REG:
            return irq_register(ebx);
        case SYS_IOPORT_ALLOW:
            return EPERM;
        case SYS_TASKS: {
            struct task *me = &task_pool[scheduler_current()];
            if (!(me->capabilities & CAP_TASK_ENUM)) return EPERM;
            unsigned int cap = ecx;
            if (cap == 0) return -1;
            if (cap > MAX_TASKS) cap = MAX_TASKS;
            if (!ua_valid(ebx, cap * sizeof(struct sys_task_info))) return -1;
            struct sys_task_info stage[MAX_TASKS];
            unsigned int n = 0;
            irq_state_t state = irq_save();
            for (int i = 0; i < task_pool_count && n < cap; i++) {
                struct task *t = &task_pool[i];
                if (t->state == TASK_FREE) continue;
                stage[n].id = (unsigned int)t->id;
                stage[n].state = (unsigned int)t->state;
                stage[n].ring = (unsigned int)t->ring;
                int j = 0;
                while (j < 15 && t->name[j]) { stage[n].name[j] = t->name[j]; j++; }
                stage[n].name[j] = 0;
                n++;
            }
            int crc = ua_copy_to(me->page_dir, ebx, stage, n * sizeof(struct sys_task_info));
            irq_restore(state);
            if (crc) return -1;
            return (int)n;
        }
        case SYS_MEMFREE:
            return task_pool[scheduler_current()].capabilities &
                CAP_RESOURCE_ADMIN ? (int)pmm_free_pages() : EPERM;
        case SYS_FORK:
            return proc_fork();
        case SYS_EXEC:
            return proc_exec(ebx, ecx);
        case SYS_EXIT:
            return proc_exit((int)ebx);
        case SYS_WAIT:
            return proc_wait((int)ebx);
        case SYS_GETPID:
            return proc_getpid();
        case SYS_KILL:
            return proc_kill((int)ebx);
        case SYS_SERVICE_REGISTER:
            return service_register(ebx);
        case SYS_SERVICE_LOOKUP:
            return service_lookup(ebx);
        case SYS_CAP_GET:
            return (int)task_pool[scheduler_current()].capabilities;
        case SYS_CAP_DROP: {
            struct task *me = &task_pool[scheduler_current()];
            irq_state_t state = irq_save();
            me->capabilities &= ~ebx;
            irq_restore(state);
            return 0;
        }
        case SYS_CAP_GRANT:
            return proc_cap_grant((int)ebx, ecx);
        case SYS_YIELD:
            arch_cpu_wait();
            return 0;
        case SYS_RECV_FROM:
            return ipc_recv_from(ebx, (struct message *)ecx);
        case SYS_IRQ_GRANT:
            return proc_irq_grant((int)ebx, ecx);
        case SYS_IOPORT_GRANT:
            return proc_ioport_grant((int)ebx, ecx, edx);
        case SYS_MMIO_GRANT:
            return proc_mmio_grant((int)ebx, ecx, edx);
        case SYS_MMIO_MAP:
            return proc_mmio_map(ebx, ecx);
        case SYS_DMA_GRANT:
            return proc_dma_grant((int)ebx, ecx, edx);
        case SYS_DMA_ALLOC:
            return proc_dma_alloc(ebx, ecx);
        case SYS_SEND_TIMEOUT:
            return ipc_send_timeout(ebx, (const struct message *)ecx, edx);
        default:
            return -1;
    }
}
