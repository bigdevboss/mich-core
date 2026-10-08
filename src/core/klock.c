#include "klock.h"
#include "irq.h"
#include "protos.h"
#include "scheduler.h"

#define KLOCK_DEPTH_MAX 4

// One record per CPU, because the only CPU that can take a lock it already
// holds is the one running the code. The boot CPU answers before the arch
// tables exist: the scheduler id falls back to its own zero record.
static struct {
    u32 depth;
    u32 levels[KLOCK_DEPTH_MAX];
    irq_state_t flags[KLOCK_DEPTH_MAX];
} klock_records[KLOCK_MAX_CPUS];

int klock_order_ok(u32 held, u32 wanted) {
    return wanted > held;
}

static u32 klock_cpu(void) {
    int cpu = scheduler_cpu_id();
    return cpu > 0 && (u32)cpu < KLOCK_MAX_CPUS ? (u32)cpu : 0;
}

u32 klock_held_level(void) {
    u32 cpu = klock_cpu();
    u32 depth = klock_records[cpu].depth;
    return depth ? klock_records[cpu].levels[depth - 1] : 0;
}

// Interrupts go off before the level is read and stay off while the lock is
// held: a tick landing inside the critical section would walk the pool this
// CPU has locked, and the unlock would never come.
void klock_acquire(struct klock *klock) {
    u32 cpu = klock_cpu();
    irq_state_t flags = irq_save();
    u32 depth = klock_records[cpu].depth;
    u32 held = depth ? klock_records[cpu].levels[depth - 1] : 0;
    if (depth >= KLOCK_DEPTH_MAX || !klock_order_ok(held, klock->level))
        panic_str("KLOCK order violation");
    spin_lock(&klock->lock);
    klock_records[cpu].levels[depth] = klock->level;
    klock_records[cpu].flags[depth] = flags;
    klock_records[cpu].depth = depth + 1;
}

void klock_release(struct klock *klock) {
    u32 cpu = klock_cpu();
    u32 depth = klock_records[cpu].depth;
    if (!depth || klock_records[cpu].levels[depth - 1] != klock->level)
        panic_str("KLOCK release out of order");
    spin_unlock(&klock->lock);
    klock_records[cpu].depth = depth - 1;
    irq_restore(klock_records[cpu].flags[depth - 1]);
}
