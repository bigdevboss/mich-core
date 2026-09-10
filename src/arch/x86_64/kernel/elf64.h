#ifndef ELF64_H
#define ELF64_H

#include "types.h"

int elf64_load(u32 space, const u8 *image, u32 size, vaddr_t *entry);

#endif
