#include "types.h"
#include "posix_abi.h"
#include "posix_time.h"
#include "rtc64.h"
#include "task.h"
#include "tests64.h"

// One timer tick in nanoseconds, the step both clocks move in.
#define TICK_NSEC (1000000000u / RTC64_TICK_HZ)

int test_posix_time64(void) {
    int valid = 1;

    // Resolution is one timer tick for both clocks, and nothing else has a
    // resolution to report.
    u64 sec = 99;
    u32 nsec = 99;
    valid = valid && posix_time_resolution(POSIX_CLOCK_REALTIME, &sec,
                                           &nsec) == 0 && sec == 0 &&
        nsec == TICK_NSEC;
    valid = valid && posix_time_resolution(POSIX_CLOCK_MONOTONIC, &sec,
                                           &nsec) == 0 && sec == 0 &&
        nsec == TICK_NSEC;
    valid = valid && posix_time_resolution(2u, &sec, &nsec) == -1;
    valid = valid && posix_time_resolution(POSIX_CLOCK_REALTIME, 0, &nsec) == -1;
    valid = valid && posix_time_resolution(POSIX_CLOCK_REALTIME, &sec, 0) == -1;

    // Both clocks read inside their ranges and move in whole ticks. The
    // wall clock is anchored at the release baseline, and a monotonic read
    // never goes backwards.
    u64 wall_sec = 0;
    u32 wall_nsec = 0;
    valid = valid && posix_time_read(POSIX_CLOCK_REALTIME, &wall_sec,
                                     &wall_nsec) == 0 &&
        wall_sec >= RTC64_SANE_EPOCH && wall_nsec < 1000000000u &&
        wall_nsec % TICK_NSEC == 0;
    u64 mono_sec = 0;
    u32 mono_nsec = 0;
    valid = valid && posix_time_read(POSIX_CLOCK_MONOTONIC, &mono_sec,
                                     &mono_nsec) == 0 &&
        mono_sec < 1000000000ull && mono_nsec < 1000000000u &&
        mono_nsec % TICK_NSEC == 0;
    u64 again_sec = 0;
    u32 again_nsec = 0;
    valid = valid && posix_time_read(POSIX_CLOCK_MONOTONIC, &again_sec,
                                     &again_nsec) == 0 &&
        (again_sec > mono_sec ||
         (again_sec == mono_sec && again_nsec >= mono_nsec));
    valid = valid && posix_time_read(2u, &sec, &nsec) == -1;
    valid = valid && posix_time_read(POSIX_CLOCK_REALTIME, 0, &nsec) == -1;
    valid = valid && posix_time_read(POSIX_CLOCK_REALTIME, &sec, 0) == -1;

    // A sleep is rounded up to whole ticks, so the park never wakes before
    // the requested interval, and a zero interval parks not at all.
    u32 ticks = 99;
    valid = valid && posix_sleep_ticks(0, 0, &ticks) == 0 && ticks == 0;
    valid = valid && posix_sleep_ticks(0, 1, &ticks) == 0 && ticks == 1;
    valid = valid && posix_sleep_ticks(0, TICK_NSEC, &ticks) == 0 &&
        ticks == 1;
    valid = valid && posix_sleep_ticks(0, TICK_NSEC + 1, &ticks) == 0 &&
        ticks == 2;
    valid = valid && posix_sleep_ticks(0, 999999999, &ticks) == 0 &&
        ticks == 100;
    valid = valid && posix_sleep_ticks(1, 0, &ticks) == 0 && ticks == 100;
    valid = valid && posix_sleep_ticks(1, 999999999, &ticks) == 0 &&
        ticks == 200;
    // A negative seconds field or a nanosecond half outside the POSIX
    // range is rejected outright.
    valid = valid && posix_sleep_ticks(-1, 0, &ticks) == -1;
    valid = valid && posix_sleep_ticks(0, -1, &ticks) == -1;
    valid = valid && posix_sleep_ticks(0, 1000000000, &ticks) == -1;
    valid = valid && posix_sleep_ticks(0, 0, 0) == -1;
    // An interval past the tick counter range parks at the counter
    // ceiling rather than wrapping back into a short sleep.
    valid = valid && posix_sleep_ticks(0x7FFFFFFFll, 0, &ticks) == 0 &&
        ticks == 0xFFFFFFFEu;

    // The timer tick wakes a parked sleeper and leaves later sleepers
    // alone. A free slot stands in for the sleeper and is returned to the
    // pool afterwards, so the probe never touches a live task.
    int probe = -1;
    for (int slot = 0; slot < task_pool_count; slot++) {
        if (task_pool[slot].state == TASK_FREE) {
            probe = slot;
            break;
        }
    }
    if (probe >= 0) {
        task_pool[probe].state = TASK_BLOCKED_SLEEP;
        task_pool[probe].sleep_deadline = 40;
        posix_time_tick(40);
        valid = valid && task_pool[probe].state == TASK_RUNNING &&
            task_pool[probe].sleep_deadline == 0;
        task_pool[probe].state = TASK_BLOCKED_SLEEP;
        task_pool[probe].sleep_deadline = 80;
        posix_time_tick(41);
        valid = valid && task_pool[probe].state == TASK_BLOCKED_SLEEP &&
            task_pool[probe].sleep_deadline == 80;
        posix_time_tick(79);
        valid = valid && task_pool[probe].state == TASK_BLOCKED_SLEEP;
        posix_time_tick(80);
        valid = valid && task_pool[probe].state == TASK_RUNNING;
        task_pool[probe].state = TASK_FREE;
        task_pool[probe].sleep_deadline = 0;
    }

    return valid ? 0 : -1;
}
