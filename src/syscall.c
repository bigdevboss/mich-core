#include "syscall.h"
#include "gfx.h"
#include "ipc.h"
#include "irq.h"
#include "tss.h"
#include "task.h"
#include "scheduler.h"
#include "pmm.h"
#include "proc.h"
#include "serial.h"
#include "uaccess.h"
#include "paging.h"

#define SYS_WRITE_CHUNK 64
#define SYS_WRITE_CHUNKS_MAX 16

void syscall_init(void) {
}

int syscall_dispatcher(unsigned int eax, unsigned int ebx, unsigned int ecx, unsigned int edx) {
    (void)edx;
    switch (eax) {
        case SYS_WRITE: {
            struct task *me = &task_pool[scheduler_current()];
            char buf[SYS_WRITE_CHUNK];
            unsigned int up = ebx;
            int rc = 0;
            for (int guard = 0; guard < SYS_WRITE_CHUNKS_MAX; guard++) {
                intr_off();
                int n = ua_copy_str(me->page_dir, buf, up, sizeof(buf));
                if (n >= 0) {
                    gfx_write(buf);
                    serial_write(buf);
                }
                intr_on();
                if (n < 0) { rc = -1; break; }
                if (n < (int)sizeof(buf) - 1 || n == 0) break;
                up += (unsigned int)n;
            }
            return rc;
        }
        case SYS_CLEAR: {
            intr_off();
            gfx_clear();
            intr_on();
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
            return tss_allow_ports(&task_pool[scheduler_current()], ebx, ecx);
        case SYS_TASKS: {
            struct task *me = &task_pool[scheduler_current()];
            unsigned int cap = ecx;
            if (cap == 0) return -1;
            if (cap > MAX_TASKS) cap = MAX_TASKS;
            if (!ua_valid(ebx, cap * sizeof(struct sys_task_info))) return -1;
            struct sys_task_info stage[MAX_TASKS];
            unsigned int n = 0;
            intr_off();
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
            intr_on();
            if (crc) return -1;
            return (int)n;
        }
        case SYS_MEMFREE:
            return (int)pmm_free_pages();
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
        default:
            return -1;
    }
}
