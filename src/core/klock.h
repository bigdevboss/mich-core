#ifndef KLOCK_H
#define KLOCK_H

#include "types.h"
#include "spinlock.h"

// Coarse locks are S2's transitional safety net: each covers a whole
// subsystem until S3 splits it. Levels fix the order a CPU takes them in,
// and a nested take at the same or a shallower level aborts: the same
// nesting under load is a deadlock, and aborting names the pair instead of
// hanging the boot. The order itself is written down in the notes.
#define KLOCK_LEVEL_POOL 1u
#define KLOCK_LEVEL_OBJECT 2u
#define KLOCK_LEVEL_PMM 3u

// The same ceiling as the CPU target, SMP64_MAX in the arch record.
#define KLOCK_MAX_CPUS 16

struct klock {
    struct spinlock lock;
    u32 level;
};

#define KLOCK_INIT(level_value) { SPINLOCK_INIT, (level_value) }

void klock_acquire(struct klock *klock);
void klock_release(struct klock *klock);
int klock_order_ok(u32 held, u32 wanted);
u32 klock_held_level(void);

#endif
