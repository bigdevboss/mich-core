#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "types.h"

//
// Ticket spinlock: unbounded wait queue (no starvation), PAUSE-padded
// spin, ACQUIRE/RELEASE ordering for the critical-section data.
//
struct spinlock {
    volatile u32 ticket;
    volatile u32 served;
};

#define SPINLOCK_INIT { 0, 0 }

static inline void spin_lock(struct spinlock *lock) {
    u32 mine = __atomic_fetch_add(&lock->ticket, 1, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&lock->served, __ATOMIC_ACQUIRE) != mine)
        __builtin_ia32_pause();
}

static inline void spin_unlock(struct spinlock *lock) {
    __atomic_fetch_add(&lock->served, 1, __ATOMIC_RELEASE);
}

#endif
