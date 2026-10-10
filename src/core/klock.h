#ifndef KLOCK_H
#define KLOCK_H

#include "types.h"
#include "spinlock.h"

// Coarse locks are S2's transitional safety net: each covers a whole
// subsystem until S3 splits it. Levels fix the order a CPU takes them in,
// and a nested take at the same or a shallower level aborts: the same
// nesting under load is a deadlock, and aborting names the pair instead of
// hanging the boot. The order itself is written down in the notes.
// The driver layer sits at the bottom because it is the caller: a domain
// teardown reaches the handles, the interrupt controllers, the IOMMU, the
// resource tables and the object table on the way out, and the manager
// reaches the supervisor, so all three take their locks before any of those.
#define KLOCK_LEVEL_MANAGER 1u
#define KLOCK_LEVEL_SUPERVISOR 2u
#define KLOCK_LEVEL_MODULE 3u
#define KLOCK_LEVEL_WAIT 4u
#define KLOCK_LEVEL_NET 5u
#define KLOCK_LEVEL_FD 6u
#define KLOCK_LEVEL_VFS 7u
#define KLOCK_LEVEL_MSI 8u
#define KLOCK_LEVEL_IOMMU 9u
#define KLOCK_LEVEL_RESOURCE 10u
#define KLOCK_LEVEL_OBJECT 11u
#define KLOCK_LEVEL_PIPE 12u
#define KLOCK_LEVEL_POOL 13u
#define KLOCK_LEVEL_PMM 14u

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
