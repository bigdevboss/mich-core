#ifndef POSIX_TIME_H
#define POSIX_TIME_H

#include "types.h"

// Clock ids the posix profile carries, with the POSIX values.
#define POSIX_CLOCK_REALTIME 0u
#define POSIX_CLOCK_MONOTONIC 1u

// Read one clock into seconds plus nanoseconds. Realtime follows the wall
// clock anchor and the tick counter inside the current second; monotonic
// counts ticks since boot and is unaffected by the wall clock. 0 on success,
// -EINVAL on a clock the profile does not carry.
int posix_time_read(u32 clock, u64 *sec, u32 *nsec);

// The resolution each clock can honestly report: one timer tick, the same
// granularity the wall clock nanoseconds move in. 0 on success, -EINVAL on
// an unknown clock.
int posix_time_resolution(u32 clock, u64 *sec, u32 *nsec);

// Validate a nanosleep interval and round it up to whole timer ticks, so a
// sleep never wakes before the caller asked. A zero or empty interval needs
// no park at all. 0 on success, -EINVAL on a negative seconds field or a
// nanosecond half outside [0, 999999999].
int posix_sleep_ticks(i64 sec, i64 nsec, u32 *ticks);

// Wake every task whose sleep deadline has passed. Called from the timer
// tick with the current tick count.
void posix_time_tick(u32 now);

#endif
