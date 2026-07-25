#include "idt.h"

struct idt_entry {
    unsigned short base_low;
    unsigned short sel;
    unsigned char zero;
    unsigned char flags;
    unsigned short base_high;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned int base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtp;

extern void irq1_handler(void);
extern void irq14_handler(void);
extern void timer_handler(void);
extern void exception_handler(void);
extern void syscall_handler(void);
extern void double_fault_handler(void);

static void idt_set_gate(unsigned char num, unsigned long base, unsigned short sel, unsigned char flags) {
    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel = sel;
    idt[num].zero = 0;
    idt[num].flags = flags;
}

void idt_init(void) {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (unsigned int)&idt;

    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, (unsigned long)exception_handler, 0x08, 0x8E);
    }

    idt_set_gate(8, (unsigned long)double_fault_handler, 0x08, 0x8E);
    idt_set_gate(32, (unsigned long)timer_handler, 0x08, 0x8E);
    idt_set_gate(33, (unsigned long)irq1_handler, 0x08, 0x8E);
    idt_set_gate(46, (unsigned long)irq14_handler, 0x08, 0x8E);
    idt_set_gate(0x80, (unsigned long)syscall_handler, 0x08, 0xEE);

    __asm__ volatile("lidt %0" : : "m"(idtp));
}
