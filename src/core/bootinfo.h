#ifndef BOOTINFO_H
#define BOOTINFO_H

#include "types.h"

#define BD_MAGIC 0x58424442u
#define BD_VERSION 2u
#define BD_VERSION_UEFI 3u
#define BD_MODULE_POSIX_PROFILE (1u << 25)
// The console wire profile: the host types into the serial port, so the
// demo may run its input stage. A plain boot has nobody typing and that
// stage would park a read nobody completes.
#define BD_MODULE_TTY_WIRE (1u << 21)

struct bd_module {
    u32 start;
    u32 end;
    u32 cmdline;
    u32 flags;
};

struct bd_info {
    u32 magic;
    u32 version;
    u64 fb_addr;
    u32 fb_pitch;
    u32 fb_width;
    u32 fb_height;
    u8 fb_bpp;
    u8 fb_type;
    u8 reserved[2];
    u32 mmap_count;
    u32 mmap_ptr;
    u32 mods_count;
    u32 mods_ptr;
    u32 rsdp_ptr;
};

#endif
