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

void syscall_init(void) {
}

int syscall_dispatcher(unsigned int eax, unsigned int ebx, unsigned int ecx, unsigned int edx) {
    (void)edx;
    switch (eax) {
        case SYS_WRITE: {
            __asm__ volatile("cli");
            gfx_write((const char *)ebx);
            serial_write((const char *)ebx);
            __asm__ volatile("sti");
            return 0;
        }
        case SYS_CLEAR: {
            __asm__ volatile("cli");
            gfx_clear();
            __asm__ volatile("sti");
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
            struct sys_task_info *out = (struct sys_task_info *)ebx;
            if (!out || ecx == 0) return -1;
            unsigned int n = 0;
            for (int i = 0; i < task_pool_count && n < ecx; i++) {
                struct task *t = &task_pool[i];
                if (t->state == TASK_FREE) continue;
                out[n].id = (unsigned int)t->id;
                out[n].state = (unsigned int)t->state;
                out[n].ring = (unsigned int)t->ring;
                int j = 0;
                while (j < 15 && t->name[j]) { out[n].name[j] = t->name[j]; j++; }
                out[n].name[j] = 0;
                n++;
            }
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
