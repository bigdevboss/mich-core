#include "gdt.h"
#include "tss.h"

struct gdt_entry {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char base_middle;
    unsigned char access;
    unsigned char granularity;
    unsigned char base_high;
} __attribute__((packed));

struct gdt_ptr {
    unsigned short limit;
    unsigned int base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_ptr gdtp;

static void gdt_set_gate(int num, unsigned long base, unsigned long limit, unsigned char access, unsigned char gran) {
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;
    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[num].access = access;
}

void gdt_init(void) {
    gdtp.limit = (sizeof(struct gdt_entry) * 6) - 1;
    gdtp.base = (unsigned int)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);
    gdt_set_gate(5, (unsigned long)&tss_full, sizeof(struct tss_full) - 1, 0x89, 0x00);

    __asm__ volatile("lgdt %0" : : "m"(gdtp));
    __asm__ volatile("mov $0x10, %ax; mov %ax, %ds; mov %ax, %es; mov %ax, %fs; mov %ax, %gs; mov %ax, %ss; ljmp $0x08, $flush");
    __asm__ volatile("flush:");
}
