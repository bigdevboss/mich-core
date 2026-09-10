#include "arch_platform.h"
#include "arch_cpu.h"

static inline void outb(unsigned short port, unsigned char value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void io_wait(void) {
    outb(0x80, 0);
}

static void pic_remap(void) {
    outb(0x20, 0x11);
    io_wait();
    outb(0xA0, 0x11);
    io_wait();
    outb(0x21, 0x20);
    io_wait();
    outb(0xA1, 0x28);
    io_wait();
    outb(0x21, 0x04);
    io_wait();
    outb(0xA1, 0x02);
    io_wait();
    outb(0x21, 0x01);
    io_wait();
    outb(0xA1, 0x01);
    io_wait();
    outb(0x21, 0x00);
    io_wait();
    outb(0xA1, 0x00);
    io_wait();
}

static void timer_init(void) {
    unsigned int divisor = 1193180 / 100;
    outb(0x43, 0x36);
    io_wait();
    outb(0x40, (unsigned char)(divisor & 0xFF));
    io_wait();
    outb(0x40, (unsigned char)((divisor >> 8) & 0xFF));
    io_wait();
}

void arch_platform_interrupts_init(void) {
    pic_remap();
    timer_init();
}

void arch_cpu_halt(void) {
    __asm__ volatile("hlt");
}

void arch_cpu_wait(void) {
    __asm__ volatile("sti; hlt" ::: "memory");
}

void arch_cpu_pause(void) {
    __asm__ volatile("pause");
}

void arch_cpu_stop(void) {
    for (;;) __asm__ volatile("cli; hlt");
}

void arch_set_address_space(paddr_t root) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(root) : "memory");
}
