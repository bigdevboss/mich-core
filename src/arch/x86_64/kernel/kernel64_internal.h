#ifndef KERNEL64_INTERNAL_H
#define KERNEL64_INTERNAL_H

#include "types.h"
#include "task.h"

struct task_context64 {
    u32 vm_space;
    u32 vm_valid;
    u64 rax;
    u64 rbx;
    u64 rcx;
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 rbp;
    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
    u64 rsp;
    u64 rip;
    u64 rflags;
    u8 fxstate[512] __attribute__((aligned(16)));
} __attribute__((aligned(16)));

extern struct task_context64 task_contexts[MAX_TASKS];
extern u32 current_task_slot;
extern u32 timer_ticks;

int serial64_user_write(u64 address);
void context_load(const struct task_context64 *ctx);
void fpu64_save(struct task_context64 *ctx);
void fpu64_load(const struct task_context64 *ctx);
u32 scheduler64_next_slot(void);
void scheduler64_set_running(u32 slot);
void scheduler64_switch(void);
u64 scheduler64_switch_count(void);
int spawn64(int parent_id, u32 caps, const char *name, u64 arg);
int fork64(void);
int exec64(u64 path, u64 argument);
void terminate64(u32 slot, int code);

#endif
