#include "gdt.h"
#include "arch_cpu.h"
#include "idt.h"
#include "gfx.h"
#include "task.h"
#include "paging.h"
#include "pmm.h"
#include "arch_task.h"
#include "syscall.h"
#include "elf.h"
#include "scheduler.h"
#include "irq.h"
#include "serial.h"
#include "service.h"
#include "protos.h"
#include "bootinfo.h"
#include "version.h"
#include "object.h"
#include "resource.h"
#include "driver.h"
#include "event.h"
#include "endpoint.h"
#include "bridge.h"
#include "arch_platform.h"

#define PAGE_SIZE 4096

extern char _bss_end;

static const char *exc_names[22] = {
    "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
    "#DF", "CSO", "#TS", "#NP", "#SS", "#GP", "#PF", "RSV",
    "#MF", "#AC", "#MC", "#XM", "#VE", "#CP"
};

static void print_dec(unsigned int v) {
    char b[11];
    int i = 10;
    b[i] = 0;
    if (!v) { serial_write_char('0'); return; }
    while (v && i) { b[--i] = (char)('0' + (v % 10)); v /= 10; }
    serial_write(&b[i]);
}

void panic(unsigned int vec, unsigned int err, unsigned int cr2, unsigned int eip) {
    serial_write("PANIC ");
    if (vec < 22) serial_write(exc_names[vec]);
    else serial_write("INT");
    serial_write(" vec=");
    print_dec(vec);
    serial_write(" err=0x");
    serial_write_hex(err);
    serial_write(" eip=0x");
    serial_write_hex(eip);
    serial_write(" cr2=0x");
    serial_write_hex(cr2);
    serial_write("\n");
    arch_cpu_stop();
}

void panic_df(void) {
    serial_write("DOUBLE FAULT\n");
    arch_cpu_stop();
}

void panic_str(const char *msg) {
    serial_write("PANIC ");
    serial_write(msg);
    serial_write("\n");
    arch_cpu_stop();
}

void kernel_main(unsigned long magic, unsigned long addr) {
    serial_init_port();
    serial_write("Mich Core ");
    serial_write(MICH_VERSION_STRING);
    serial_write(": serial alive\n");

    gdt_init();
    serial_write("Mich: GDT done\n");

    paging_init();
    paging_enable();
    serial_write("Mich: paging enabled\n");

    struct bd_info *bi = (struct bd_info *)addr;
    if (magic != BD_MAGIC || bi->magic != BD_MAGIC || bi->version != BD_VERSION) {
        serial_write("Mich: needs bigdevboot protocol v2, halting\n");
        arch_cpu_stop();
    }
    serial_write("Mich: bigdevboot protocol\n");
    if (!bi->mmap_ptr || !bi->mmap_count || bi->mmap_count > 32)
        panic_str("invalid memory map");
    if (bi->mods_count > 8 || (bi->mods_count && !bi->mods_ptr))
        panic_str("invalid module table");

    pmm_init((uptr_t)bi->mmap_ptr,
             (usize_t)bi->mmap_count * 24,
             24,
             (unsigned int)&_bss_end);
    serial_write("Mich: PMM done, free=");
    serial_write_hex(pmm_free_pages());
    serial_write("\n");

    if (!bi->fb_addr) {
        serial_write("Mich: NO FRAMEBUFFER, halting\n");
        arch_cpu_stop();
    }
    if (bi->fb_addr > 0xFFFFFFFFULL || bi->fb_width < 8 || bi->fb_height < 16 ||
        bi->fb_width > 16384 || bi->fb_height > 16384 ||
        (bi->fb_bpp != 24 && bi->fb_bpp != 32))
        panic_str("invalid framebuffer");
    u64 min_pitch = (u64)bi->fb_width * (bi->fb_bpp / 8);
    u64 fb_bytes = (u64)bi->fb_pitch * bi->fb_height;
    if (bi->fb_pitch < min_pitch || !fb_bytes || fb_bytes > 0xFFFFFFFFULL ||
        bi->fb_addr + fb_bytes > 0x100000000ULL)
        panic_str("invalid framebuffer size");
    unsigned int *fb = (unsigned int *)(uptr_t)bi->fb_addr;
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
    unsigned int mod_flags[8];
    unsigned int total_mods = bi->mods_count;
    int nmods = (int)total_mods;
    int i;
    for (i = 0; i < nmods; i++) {
        if (bm[i].end <= bm[i].start || !bm[i].cmdline)
            panic_str("invalid boot module");
        pmm_reserve(bm[i].start, bm[i].end - bm[i].start);
        mod_starts[i] = bm[i].start;
        mod_sizes[i] = bm[i].end - bm[i].start;
        mod_names[i] = bm[i].cmdline;
        mod_flags[i] = bm[i].flags;
    }

    paging_init_scratch();

    unsigned int fb_phys = (unsigned int)(uptr_t)fb;
    unsigned int fb_size = (unsigned int)fb_bytes;
    pmm_reserve(fb_phys, fb_size);
    unsigned int map_start = fb_phys & ~(PAGE_SIZE - 1);
    u64 map_end64 = ((u64)fb_phys + fb_size + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
    for (u64 page = map_start; page < map_end64; page += PAGE_SIZE) {
        if (paging_map_kernel((u32)page, (u32)page, 0x2) != 0)
            panic_str("framebuffer map failed");
    }
    serial_write("Mich: framebuffer mapped\n");

    gfx_init(fb, fb_width, fb_height, fb_pitch, fb_bpp);
    serial_write("Mich: GFX init done\n");

    arch_task_init();
    serial_write("Mich: TSS done\n");

    arch_platform_interrupts_init();
    idt_init();
    irq_init();
    syscall_init();

    scheduler_init_idle(paging_kernel_dir_phys());
    service_init();
    object_init();
    resource_init();
    driver_init();
    event_init(0);
    endpoint_init();
    bridge_init();

    int loaded = 0;
    for (i = 0; i < nmods; i++) {
        int rc = elf_load((unsigned int *)mod_starts[i], mod_sizes[i],
                          (const char *)(unsigned long)mod_names[i], mod_flags[i]);
        if (rc == 0) {
            loaded++;
        } else {
            serial_write("Mich: module ");
            print_dec((unsigned int)i);
            serial_write(" load fail rc=");
            if (rc < 0) {
                serial_write_char('-');
                print_dec((unsigned int)(-rc));
            } else {
                print_dec((unsigned int)rc);
            }
            serial_write("\n");
        }
    }

    if (!loaded) {
        serial_write("Mich Core " MICH_VERSION_STRING ": no modules, idling\n");
    }

    serial_write("Mich: tasks ready, enabling preemption\n");
    intr_on();
    for (;;) {
        arch_cpu_halt();
    }
}
