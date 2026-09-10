#include "serial.h"

#define COM1 0x3F8

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void serial_init_port(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static int serial_is_transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_write_char(char c) {
    while (!serial_is_transmit_empty());
    outb(COM1, c);
}

void serial_write(const char *str) {
    for (; *str; str++) serial_write_char(*str);
}

void serial_write_hex(unsigned int v) {
    for (int i = 28; i >= 0; i -= 4) {
        unsigned int nibble = (v >> i) & 0xF;
        serial_write_char(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
    }
}
