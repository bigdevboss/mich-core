#ifndef RUNTIME64_H
#define RUNTIME64_H

#include "types.h"

struct bd_info;

struct exception_frame64 {
    u64 r15;
    u64 r14;
    u64 r13;
    u64 r12;
    u64 r11;
    u64 r10;
    u64 r9;
    u64 r8;
    u64 rbp;
    u64 rdi;
    u64 rsi;
    u64 rdx;
    u64 rcx;
    u64 rbx;
    u64 rax;
    u64 vector;
    u64 error;
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};

struct interrupt_frame64;

void task64_set_result(u32 slot, i64 result);
i64 task64_block_switch(void);
void exception64_dispatch(struct exception_frame64 *frame);
void timer64_dispatch(struct interrupt_frame64 *frame);
void irq64_dispatch(u64 vector);
u64 syscall64_validate_return(u64 result);
u64 syscall64_dispatch(u64 number, u64 arg0, u64 arg1, u64 arg2);
void kernel64_main(u32 magic, struct bd_info *info);

#endif
