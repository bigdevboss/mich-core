#ifndef RTC64_H
#define RTC64_H

#include "types.h"

// Unix seconds at the 0.1.0 release baseline (2026-09-24 08:53:20 UTC). A CMOS
// clock reporting anything earlier has lost its battery, and every certificate
// date check would then reject valid certificates.
#define RTC64_SANE_EPOCH 1790240000ull

#define RTC64_TICK_HZ 100u

// Reads the CMOS clock once and anchors it to the timer tick. The chip is only
// touched here: a second reader would have to interleave index and data port
// writes, which is not safe once more than one CPU is running.
// 0 on success, -1 when the chip reports a date that cannot be converted.
int rtc64_init(void);

// Unix seconds, UTC, derived from the boot anchor plus elapsed ticks. Returns
// 0 when the clock was never anchored.
u64 rtc64_wall_clock(void);

// Pure civil-date arithmetic, no CMOS access. Exposed so the known-answer test
// can cover leap years and month boundaries without hardware.
u64 rtc64_unix_seconds(u32 year, u32 month, u32 day,
                       u32 hour, u32 minute, u32 second);

#endif
