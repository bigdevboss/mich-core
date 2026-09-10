#include "types.h"
#include "idt64.h"
#include "apic64.h"
#include "smp64.h"
#include "vector64.h"

struct idt_entry64 {
    u16 offset0;
    u16 selector;
    u8 ist;
    u8 flags;
    u16 offset1;
    u32 offset2;
    u32 zero;
} __attribute__((packed));

struct idtr64 {
    u16 limit;
    u64 base;
} __attribute__((packed));

static struct idt_entry64 idt[256] __attribute__((aligned(16)));
extern u64 exception64_table[];
extern void timer64_entry(void);
extern void spurious64_entry(void);
extern u64 irq64_table[];
extern u64 msi64_table[];
extern u64 smp64_ipi_table[];

static void idt_set(u32 vector, u64 handler, u8 ist) {
    idt[vector].offset0 = handler & 0xFFFF;
    idt[vector].selector = 0x08;
    idt[vector].ist = ist & 7;
    idt[vector].flags = 0x8E;
    idt[vector].offset1 = (handler >> 16) & 0xFFFF;
    idt[vector].offset2 = handler >> 32;
    idt[vector].zero = 0;
}

void idt64_init(void) {
    for (u32 i = 0; i < 256; i++) {
        idt[i].offset0 = 0;
        idt[i].selector = 0;
        idt[i].ist = 0;
        idt[i].flags = 0;
        idt[i].offset1 = 0;
        idt[i].offset2 = 0;
        idt[i].zero = 0;
    }
    for (u32 i = 0; i < 32; i++) idt_set(i, exception64_table[i], 0);
    idt_set(8, exception64_table[8], 1);
    for (u32 vector = 0x30; vector < 0x40; vector++)
        idt_set(vector, irq64_table[vector - 0x30], 0);
    for (u32 vector = VECTOR64_MSI_FIRST;
         vector <= VECTOR64_MSI_LAST; vector++)
        idt_set(vector, msi64_table[vector - VECTOR64_MSI_FIRST], 0);
    for (u32 vector = SMP64_IPI_FIRST; vector <= SMP64_IPI_LAST; vector++)
        idt_set(vector, smp64_ipi_table[vector - SMP64_IPI_FIRST], 0);
    idt_set(APIC64_TIMER_VECTOR, (u64)(uptr_t)timer64_entry, 0);
    idt_set(APIC64_SPURIOUS_VECTOR, (u64)(uptr_t)spurious64_entry, 0);
    idt64_reload();
}

void idt64_reload(void) {
    struct idtr64 ptr;
    ptr.limit = sizeof(idt) - 1;
    ptr.base = (u64)(uptr_t)idt;
    __asm__ volatile("lidt %0" : : "m"(ptr));
}

