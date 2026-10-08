#ifndef VECTOR64_H
#define VECTOR64_H

#include "types.h"
#include "klock.h"

#define VECTOR64_MSI_FIRST 0x50
#define VECTOR64_MSI_LAST 0xDF

// The vector table is what the MSI and MSI-X paths allocate from and hand to
// the same destination logic, so it shares their lock: one programming lock
// for the whole message-interrupt surface.
extern struct klock irq_program_klock;

void vector64_init(void);
int vector64_allocate(uptr_t owner);
int vector64_allocate_group(uptr_t owner, u32 count, u8 *vectors);
int vector64_release_group(const u8 *vectors, u32 count, uptr_t owner);
int vector64_release(u8 vector, uptr_t owner);
u32 vector64_available(void);
int vector64_transfer(u8 vector, uptr_t old_owner, uptr_t new_owner);
uptr_t vector64_owner(u8 vector);

int vector64_allocate_locked(uptr_t owner);
int vector64_allocate_group_locked(uptr_t owner, u32 count, u8 *vectors);
int vector64_release_locked(u8 vector, uptr_t owner);
int vector64_transfer_locked(u8 vector, uptr_t old_owner, uptr_t new_owner);
uptr_t vector64_owner_locked(u8 vector);

#endif
