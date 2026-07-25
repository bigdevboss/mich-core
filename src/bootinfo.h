#ifndef BOOTINFO_H
#define BOOTINFO_H

#define BD_MAGIC 0x58424442
#define BD_VERSION 1

struct bd_module {
    unsigned int start;
    unsigned int end;
    unsigned int cmdline;
};

struct bd_info {
    unsigned int magic;
    unsigned int version;
    unsigned long long fb_addr;
    unsigned int fb_pitch;
    unsigned int fb_width;
    unsigned int fb_height;
    unsigned char fb_bpp;
    unsigned char fb_type;
    unsigned char reserved[2];
    unsigned int mmap_count;
    unsigned int mmap_ptr;
    unsigned int mods_count;
    unsigned int mods_ptr;
};

#endif
