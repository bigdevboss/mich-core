#include "panic64.h"
#include "runtime64.h"
#include "serial64.h"
#include "scheduler.h"
#include "task.h"

static volatile u32 panic_active;

static void field(const char *name, u64 value) {
    serial64_write(name);
    serial64_hex(value);
    serial64_write("\n");
}

static void banner(const char *reason) {
    serial64_write("\n================ MICH KERNEL PANIC ================\n");
    serial64_write("reason: ");
    serial64_write(reason ? reason : "unknown");
    serial64_write("\n");
    int current = scheduler_current();
    if (current >= 0 && current < task_pool_count) {
        field("task slot: 0x", (u32)current);
        field("task pid:  0x", (u32)task_pool[current].id);
    }
    u64 cr2;
    u64 cr3;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    field("cr2:       0x", cr2);
    field("cr3:       0x", cr3);
}

static void stop(void) __attribute__((noreturn));

static void stop(void) {
    serial64_write("===================================================\n");
    for (;;) __asm__ volatile("cli; hlt");
}

void panic64_halt(const char *reason) {
    __asm__ volatile("cli" ::: "memory");
    if (__atomic_exchange_n(&panic_active, 1, __ATOMIC_SEQ_CST)) {
        serial64_write("\nMICH RECURSIVE PANIC\n");
        stop();
    }
    banner(reason);
    stop();
}

void panic64_frame(const char *reason,
                   const struct exception_frame64 *frame) {
    __asm__ volatile("cli" ::: "memory");
    if (__atomic_exchange_n(&panic_active, 1, __ATOMIC_SEQ_CST)) {
        serial64_write("\nMICH RECURSIVE PANIC\n");
        stop();
    }
    banner(reason);
    if (!frame) stop();
    field("vector:    0x", frame->vector);
    field("error:     0x", frame->error);
    field("rip:       0x", frame->rip);
    field("cs:        0x", frame->cs);
    field("rflags:    0x", frame->rflags);
    field("rsp:       0x", frame->rsp);
    field("ss:        0x", frame->ss);
    field("rax:       0x", frame->rax);
    field("rbx:       0x", frame->rbx);
    field("rcx:       0x", frame->rcx);
    field("rdx:       0x", frame->rdx);
    field("rsi:       0x", frame->rsi);
    field("rdi:       0x", frame->rdi);
    field("rbp:       0x", frame->rbp);
    field("r8:        0x", frame->r8);
    field("r9:        0x", frame->r9);
    field("r10:       0x", frame->r10);
    field("r11:       0x", frame->r11);
    field("r12:       0x", frame->r12);
    field("r13:       0x", frame->r13);
    field("r14:       0x", frame->r14);
    field("r15:       0x", frame->r15);
    stop();
}
