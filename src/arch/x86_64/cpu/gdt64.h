#ifndef GDT64_H
#define GDT64_H

#include "types.h"

#define GDT64_CPU_MAX 8
#define GDT64_KERNEL_CS 0x08
#define GDT64_KERNEL_DS 0x10
#define GDT64_USER_DS 0x1B
#define GDT64_USER_CS 0x23
#define GDT64_TSS_SELECTOR 0x28
#define GDT64_TSS_STRIDE 0x10

void gdt64_init(void);
void gdt64_load_cpu(u32 cpu);
u64 gdt64_base(void);
u16 gdt64_tss_selector(u32 cpu);
int gdt64_stack_ok(void);
int gdt64_tss_ok(void);
int gdt64_on_ring0(u32 cpu, u64 rsp);

#endif
