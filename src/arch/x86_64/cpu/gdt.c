#include "types.h"
#include "gdt64.h"

#define GDT64_ENTRY_KERNEL_CS 1
#define GDT64_ENTRY_KERNEL_DS 2
#define GDT64_ENTRY_USER_DS 3
#define GDT64_ENTRY_USER_CS 4
#define GDT64_ENTRY_TSS 5
#define GDT64_TSS_SLOTS 2
#define GDT64_DESC_COUNT (GDT64_ENTRY_TSS + GDT64_CPU_MAX * GDT64_TSS_SLOTS)
#define GDT64_DESC_KERNEL_CS 0x00AF9A000000FFFFULL
#define GDT64_DESC_KERNEL_DS 0x00CF92000000FFFFULL
#define GDT64_DESC_USER_DS 0x00CFF2000000FFFFULL
#define GDT64_DESC_USER_CS 0x00AFFA000000FFFFULL
#define GDT64_TSS_TYPE 0x89ULL
#define GDT64_RING0_CANARY 0x4D4943485354414BULL
#define GDT64_IST1_CANARY 0x4D49434849535431ULL
#define GDT64_RING0_STACK 32768

struct gdtr64 {
    u16 limit;
    u64 base;
} __attribute__((packed));

struct tss64 {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist1;
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 reserved2;
    u16 reserved3;
    u16 iomap;
} __attribute__((packed));

static u64 gdt[GDT64_DESC_COUNT] __attribute__((aligned(16)));
static struct tss64 tss[GDT64_CPU_MAX] __attribute__((aligned(16)));

struct ring0_stack64 {
    u64 canary;
    u64 reserved;
    u8 bytes[GDT64_RING0_STACK];
} __attribute__((aligned(16)));

static struct ring0_stack64 ring0_stack[GDT64_CPU_MAX];
static struct ring0_stack64 ist1_stack[GDT64_CPU_MAX];

extern void gdt64_load(const struct gdtr64 *ptr);
extern void gdt64_ltr(u16 selector);

static u64 gdt64_stack_top(const struct ring0_stack64 *stack) {
    return (u64)(uptr_t)(stack->bytes + sizeof(stack->bytes));
}

static void gdt64_clear_tss(struct tss64 *cpu_tss) {
    u8 *bytes = (u8 *)cpu_tss;
    for (u32 index = 0; index < sizeof(*cpu_tss); index++)
        bytes[index] = 0;
}

static void gdt64_set_tss(u32 cpu, const struct tss64 *cpu_tss) {
    u64 base = (u64)(uptr_t)cpu_tss;
    u64 limit = sizeof(*cpu_tss) - 1;
    u32 index = GDT64_ENTRY_TSS + cpu * GDT64_TSS_SLOTS;
    gdt[index] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) |
                 (GDT64_TSS_TYPE << 40) | (((limit >> 16) & 0xF) << 48) |
                 (((base >> 24) & 0xFF) << 56);
    gdt[index + 1] = base >> 32;
}

u16 gdt64_tss_selector(u32 cpu) {
    return (u16)(GDT64_TSS_SELECTOR + cpu * GDT64_TSS_STRIDE);
}

void gdt64_load_cpu(u32 cpu) {
    struct gdtr64 ptr;
    ptr.limit = sizeof(gdt) - 1;
    ptr.base = (u64)(uptr_t)gdt;
    gdt64_load(&ptr);
    gdt64_ltr(gdt64_tss_selector(cpu));
}

void gdt64_init(void) {
    gdt[0] = 0;
    gdt[GDT64_ENTRY_KERNEL_CS] = GDT64_DESC_KERNEL_CS;
    gdt[GDT64_ENTRY_KERNEL_DS] = GDT64_DESC_KERNEL_DS;
    gdt[GDT64_ENTRY_USER_DS] = GDT64_DESC_USER_DS;
    gdt[GDT64_ENTRY_USER_CS] = GDT64_DESC_USER_CS;
    for (u32 cpu = 0; cpu < GDT64_CPU_MAX; cpu++) {
        ring0_stack[cpu].canary = GDT64_RING0_CANARY;
        ist1_stack[cpu].canary = GDT64_IST1_CANARY;
        gdt64_clear_tss(&tss[cpu]);
        tss[cpu].rsp0 = gdt64_stack_top(&ring0_stack[cpu]);
        tss[cpu].ist1 = gdt64_stack_top(&ist1_stack[cpu]);
        tss[cpu].iomap = sizeof(tss[cpu]);
        gdt64_set_tss(cpu, &tss[cpu]);
    }
    gdt64_load_cpu(0);
}

u64 gdt64_base(void) {
    return (u64)(uptr_t)gdt;
}

int gdt64_stack_ok(void) {
    for (u32 cpu = 0; cpu < GDT64_CPU_MAX; cpu++) {
        if (ring0_stack[cpu].canary != GDT64_RING0_CANARY)
            return 0;
        if (ist1_stack[cpu].canary != GDT64_IST1_CANARY)
            return 0;
    }
    return 1;
}

int gdt64_on_ring0(u32 cpu, u64 rsp) {
    u64 base;
    u64 end;
    if (cpu >= GDT64_CPU_MAX) return 0;
    base = (u64)(uptr_t)ring0_stack[cpu].bytes;
    end = gdt64_stack_top(&ring0_stack[cpu]);
    return rsp > base && rsp <= end;
}

int gdt64_tss_ok(void) {
    if (!gdt64_stack_ok()) return 0;
    for (u32 cpu = 0; cpu < GDT64_CPU_MAX; cpu++) {
        if (tss[cpu].rsp0 != gdt64_stack_top(&ring0_stack[cpu]))
            return 0;
        if (tss[cpu].ist1 != gdt64_stack_top(&ist1_stack[cpu]))
            return 0;
        for (u32 other = 0; other < cpu; other++) {
            if (tss[cpu].rsp0 == tss[other].rsp0) return 0;
            if (tss[cpu].ist1 == tss[other].ist1) return 0;
        }
    }
    return 1;
}
