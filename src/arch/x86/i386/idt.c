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

extern void timer_handler(void);
extern void exc_unknown(void);
extern void syscall_handler(void);
extern unsigned int exc_table[];
extern unsigned int irq_table[];

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
        idt_set_gate(i, (unsigned long)exc_unknown, 0x08, 0x8E);
    }
    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, (unsigned long)exc_table[i], 0x08, 0x8E);
    }

    idt_set_gate(8, 0, 0x30, 0x85);
    idt_set_gate(32, (unsigned long)timer_handler, 0x08, 0x8E);
    for (int i = 1; i < 16; i++) {
        idt_set_gate((unsigned char)(32 + i),
                     (unsigned long)irq_table[i - 1], 0x08, 0x8E);
    }
    idt_set_gate(0x80, (unsigned long)syscall_handler, 0x08, 0xEE);

    __asm__ volatile("lidt %0" : : "m"(idtp));
}
