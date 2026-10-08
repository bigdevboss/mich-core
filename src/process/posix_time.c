#include "posix_time.h"
#include "posix_abi.h"
#include "runtime64.h"
#include "rtc64.h"
#include "task.h"

// The monotonic clock counts the same timer ticks the scheduler does, so it
// reads the counter the kernel core owns rather than a wall clock anchor.
extern u32 timer_ticks;

// One timer tick in nanoseconds, the resolution both clocks move in.
#define POSIX_TIME_TICK_NSEC (1000000000u / RTC64_TICK_HZ)

int posix_time_read(u32 clock, u64 *sec, u32 *nsec) {
    if (!sec || !nsec) return -1;
    if (clock == POSIX_CLOCK_REALTIME) {
        *sec = rtc64_wall_clock();
        *nsec = rtc64_wall_clock_nsec();
        return 0;
    }
    if (clock == POSIX_CLOCK_MONOTONIC) {
        // Boot is tick zero, so monotonic time is the raw tick count: the
        // whole seconds and the nanoseconds inside the current second.
        *sec = timer_ticks / RTC64_TICK_HZ;
        *nsec = (timer_ticks % RTC64_TICK_HZ) * POSIX_TIME_TICK_NSEC;
        return 0;
    }
    return -1;
}

int posix_time_resolution(u32 clock, u64 *sec, u32 *nsec) {
    if (!sec || !nsec) return -1;
    if (clock != POSIX_CLOCK_REALTIME && clock != POSIX_CLOCK_MONOTONIC)
        return -1;
    *sec = 0;
    *nsec = POSIX_TIME_TICK_NSEC;
    return 0;
}

int posix_sleep_ticks(i64 sec, i64 nsec, u32 *ticks) {
    if (!ticks) return -1;
    if (sec < 0 || nsec < 0 || nsec > 999999999) return -1;
    // Round up so the park never wakes before the requested interval: a
    // one-nanosecond ask still waits out a full tick. Seconds dominate the
    // arithmetic in ticks, which keeps the nanosecond sum far from
    // overflowing, and an interval past the tick counter's range simply
    // parks at the counter ceiling.
    u64 count = (u64)sec * RTC64_TICK_HZ +
        ((u64)nsec + POSIX_TIME_TICK_NSEC - 1u) / POSIX_TIME_TICK_NSEC;
    if (count > 0xFFFFFFFEull) count = 0xFFFFFFFEull;
    *ticks = (u32)count;
    return 0;
}

void posix_time_tick(u32 now) {
    // The walk holds the pool lock: it reads the same states the spawn and
    // the exit write, and a slot freed under it would be woken as a task
    // that never parked. The body stays on pool fields, so nothing deeper
    // is taken while the lock is held.
    task_pool_lock();
    for (int slot = 0; slot < task_pool_count; slot++) {
        struct task *task = &task_pool[slot];
        if (task->state != TASK_BLOCKED_SLEEP) continue;
        if ((i32)(now - task->sleep_deadline) < 0) continue;
        task_sleep_expire_locked(task);
        // The parked dispatch frame was abandoned on the switch, so the
        // wake publishes the return value the way the event wake does.
        task64_set_result((u32)slot, 0);
    }
    task_pool_unlock();
}
