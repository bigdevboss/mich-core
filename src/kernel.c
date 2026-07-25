#include "gdt.h"
#include "idt.h"
#include "gfx.h"
#include "task.h"
#include "paging.h"
#include "pmm.h"
#include "tss.h"
#include "syscall.h"
#include "elf.h"
#include "scheduler.h"
#include "irq.h"
#include "serial.h"
#include "bootinfo.h"

#define COM1 0x3F8
#define PAGE_SIZE 4096

extern char _bss_end;

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

void panic(unsigned int err, unsigned int addr) {
    unsigned int tmp;
    int i;
    serial_write("PANIC code=");
    serial_write_char('0' + (err / 10));
    serial_write_char('0' + (err % 10));
    serial_write(" addr=");
    tmp = addr;
    for (i = 28; i >= 0; i -= 4) {
        unsigned int nibble = (tmp >> i) & 0xF;
        serial_write_char(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
    }
    serial_write("\n");
    for (;;) __asm__ volatile("hlt");
}

void panic_df(void) {
    serial_write("DOUBLE FAULT\n");
    for (;;) __asm__ volatile("hlt");
}

static void pic_remap(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
}

static void timer_init(void) {
    unsigned int divisor = 1193180 / 100;
    outb(0x43, 0x36);
    outb(0x40, (unsigned char)(divisor & 0xFF));
    outb(0x40, (unsigned char)((divisor >> 8) & 0xFF));
}

void kernel_main(unsigned long magic, unsigned long addr) {
    serial_init_port();
    serial_write("Mich: serial alive\n");

    gdt_init();
    serial_write("Mich: GDT done\n");

    paging_init();
    paging_enable();
    serial_write("Mich: paging enabled\n");

    struct bd_info *bi = (struct bd_info *)addr;
    if (magic != BD_MAGIC || bi->magic != BD_MAGIC) {
        serial_write("Mich: needs bigdevboot (BDBX), halting\n");
        for (;;) __asm__ volatile("hlt");
    }
    serial_write("Mich: bigdevboot protocol\n");

    pmm_init((unsigned long)bi->mmap_ptr,
             (unsigned long)(bi->mmap_count * 24),
             24,
             (unsigned int)&_bss_end);
    serial_write("Mich: PMM done, free=");
    serial_write_hex(pmm_free_pages());
    serial_write("\n");

    if (!bi->fb_addr) {
        serial_write("Mich: NO FRAMEBUFFER, halting\n");
        for (;;) __asm__ volatile("hlt");
    }
    unsigned int *fb = (unsigned int *)(unsigned long)bi->fb_addr;
    unsigned int fb_width = bi->fb_width;
    unsigned int fb_height = bi->fb_height;
    unsigned int fb_pitch = bi->fb_pitch;
    unsigned char fb_bpp = bi->fb_bpp;
    serial_write("Mich: framebuffer found, bpp=");
    serial_write_char('0' + (fb_bpp / 10));
    serial_write_char('0' + (fb_bpp % 10));
    serial_write("\n");

    struct bd_module *bm = (struct bd_module *)bi->mods_ptr;
    unsigned int mod_starts[8];
    unsigned int mod_sizes[8];
    unsigned int mod_names[8];
    int nmods = (int)bi->mods_count;
    if (nmods > 8) nmods = 8;
    int i;
    for (i = 0; i < nmods; i++) {
        pmm_reserve(bm[i].start, bm[i].end - bm[i].start);
        mod_starts[i] = bm[i].start;
        mod_sizes[i] = bm[i].end - bm[i].start;
        mod_names[i] = bm[i].cmdline;
    }

    paging_init_scratch();

    unsigned int fb_phys = (unsigned int)fb;
    unsigned int fb_size = fb_pitch * fb_height;
    pmm_reserve(fb_phys, fb_size);
    for (unsigned int offset = 0; offset < fb_size; offset += PAGE_SIZE) {
        paging_map_kernel(fb_phys + offset, fb_phys + offset, 0x2);
    }
    serial_write("Mich: framebuffer mapped\n");

    gfx_init(fb, fb_width, fb_height, fb_pitch, fb_bpp);
    serial_write("Mich: GFX init done\n");

    tss_init();
    serial_write("Mich: TSS done\n");

    pic_remap();
    idt_init();
    irq_init();
    timer_init();
    syscall_init();

    scheduler_init_idle(paging_kernel_dir_phys());

    int loaded = 0;
    for (i = 0; i < nmods; i++) {
        if (elf_load((unsigned int *)mod_starts[i], mod_sizes[i],
                     (const char *)(unsigned long)mod_names[i]) == 0)
            loaded++;
    }

    if (!loaded) {
        serial_write("Mich Core 2.0: no modules, idling\n");
    }

    serial_write("Mich: tasks ready, enabling preemption\n");
    __asm__ volatile("sti");
    for (;;) {
        __asm__ volatile("hlt");
    }
}
