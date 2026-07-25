#ifndef MULTIBOOT_MMAP_H
#define MULTIBOOT_MMAP_H

struct multiboot_mmap_entry {
    unsigned long long addr;
    unsigned long long len;
    unsigned int type;
    unsigned int reserved;
} __attribute__((packed));

#endif
