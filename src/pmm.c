#include "pmm.h"
#include "multiboot_mmap.h"

#define PAGE_SIZE 4096
#define BITMAP_SIZE 0x4000

static unsigned char bitmap[BITMAP_SIZE];
static unsigned int free_pages = 0;
static unsigned int max_page = 0;

void pmm_init(unsigned long mmap_addr, unsigned long mmap_length, unsigned int entry_size, unsigned int kernel_end) {
    for (unsigned int i = 0; i < BITMAP_SIZE; i++) bitmap[i] = 0xFF;

    struct multiboot_mmap_entry *entry = (struct multiboot_mmap_entry *)mmap_addr;
    while ((unsigned long)entry < mmap_addr + mmap_length) {
        if (entry->type == 1) {
            for (unsigned long long off = 0; off < entry->len; off += PAGE_SIZE) {
                unsigned long long page = (entry->addr + off) / PAGE_SIZE;
                if (page < BITMAP_SIZE * 8) {
                    bitmap[page / 8] &= ~(1u << (page % 8));
                    free_pages++;
                    if ((unsigned int)page > max_page) max_page = (unsigned int)page;
                }
            }
        }
        entry = (struct multiboot_mmap_entry *)((unsigned char *)entry + entry_size);
    }

    pmm_reserve(0, 0x100000);
    pmm_reserve(0x100000, kernel_end - 0x100000);
}

void pmm_reserve(unsigned long addr, unsigned long len) {
    unsigned long a = addr & ~0xFFFL;
    unsigned long end = (addr + len + PAGE_SIZE - 1) & ~0xFFFL;
    for (; a < end; a += PAGE_SIZE) {
        unsigned long page = a / PAGE_SIZE;
        if (page < BITMAP_SIZE * 8 && !(bitmap[page / 8] & (1u << (page % 8)))) {
            bitmap[page / 8] |= (1u << (page % 8));
            free_pages--;
        }
    }
}

unsigned int pmm_alloc_page(void) {
    for (unsigned int i = 0; i < BITMAP_SIZE * 8; i++) {
        if (!(bitmap[i / 8] & (1u << (i % 8)))) {
            bitmap[i / 8] |= (1u << (i % 8));
            free_pages--;
            unsigned int phys = i * PAGE_SIZE;
            if (phys < 0x1000000) {
                unsigned char *p = (unsigned char *)phys;
                for (unsigned int j = 0; j < PAGE_SIZE; j++) p[j] = 0;
            }
            return phys;
        }
    }
    return 0;
}

unsigned int pmm_alloc_page_low(void) {
    for (unsigned int i = 0; i < 0x1000000 / PAGE_SIZE; i++) {
        if (!(bitmap[i / 8] & (1u << (i % 8)))) {
            bitmap[i / 8] |= (1u << (i % 8));
            free_pages--;
            unsigned int phys = i * PAGE_SIZE;
            unsigned char *p = (unsigned char *)phys;
            for (unsigned int j = 0; j < PAGE_SIZE; j++) p[j] = 0;
            return phys;
        }
    }
    return 0;
}

void pmm_free_page(unsigned int addr) {
    unsigned int page = addr / PAGE_SIZE;
    if (page < BITMAP_SIZE * 8 && (bitmap[page / 8] & (1u << (page % 8)))) {
        bitmap[page / 8] &= ~(1u << (page % 8));
        free_pages++;
    }
}

unsigned int pmm_free_pages(void) {
    return free_pages;
}
