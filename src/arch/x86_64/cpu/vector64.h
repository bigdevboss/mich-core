#ifndef VECTOR64_H
#define VECTOR64_H

#include "types.h"

#define VECTOR64_MSI_FIRST 0x50
#define VECTOR64_MSI_LAST 0xDF

void vector64_init(void);
int vector64_allocate(uptr_t owner);
int vector64_allocate_group(uptr_t owner, u32 count, u8 *vectors);
int vector64_release_group(const u8 *vectors, u32 count, uptr_t owner);
int vector64_release(u8 vector, uptr_t owner);
u32 vector64_available(void);
int vector64_transfer(u8 vector, uptr_t old_owner, uptr_t new_owner);
uptr_t vector64_owner(u8 vector);

#endif
