#include "types.h"
#include "rtc64.h"
#include "tests64.h"

struct rtc_vector {
    u32 year;
    u32 month;
    u32 day;
    u32 hour;
    u32 minute;
    u32 second;
    u64 expected;
};

// Dates that break naive conversions: the epoch itself, the century rule that
// makes 1900 and 2100 ordinary years while 2000 is a leap year, both sides of
// a leap day, month-length boundaries, and the release baseline the kernel
// compares against when it decides the board battery is dead.
static const struct rtc_vector vectors[] = {
    {1970, 1, 1, 0, 0, 0, 0ull},
    {1970, 1, 1, 0, 0, 1, 1ull},
    {1970, 1, 2, 0, 0, 0, 86400ull},
    {1970, 12, 31, 23, 59, 59, 31535999ull},
    {1971, 1, 1, 0, 0, 0, 31536000ull},
    {1972, 2, 28, 23, 59, 59, 68169599ull},
    {1972, 2, 29, 0, 0, 0, 68169600ull},
    {1972, 3, 1, 0, 0, 0, 68256000ull},
    {2000, 2, 29, 12, 0, 0, 951825600ull},
    {2000, 3, 1, 0, 0, 0, 951868800ull},
    {2001, 9, 9, 1, 46, 40, 1000000000ull},
    {2024, 2, 29, 23, 59, 59, 1709251199ull},
    {2024, 3, 1, 0, 0, 0, 1709251200ull},
    {2026, 9, 24, 8, 53, 20, RTC64_SANE_EPOCH},
    {2038, 1, 19, 3, 14, 7, 2147483647ull},
    {2038, 1, 19, 3, 14, 8, 2147483648ull},
    {2100, 2, 28, 0, 0, 0, 4107456000ull},
    {2100, 3, 1, 0, 0, 0, 4107542400ull},
};

int test_rtc64(void) {
    int valid = 1;
    for (u32 index = 0; index < sizeof(vectors) / sizeof(vectors[0]); index++) {
        const struct rtc_vector *vector = &vectors[index];
        if (rtc64_unix_seconds(vector->year, vector->month, vector->day,
                               vector->hour, vector->minute,
                               vector->second) != vector->expected)
            valid = 0;
    }

    // 2038 has to keep counting: the value is 64-bit and must not wrap where a
    // 32-bit time_t would.
    valid = valid &&
        rtc64_unix_seconds(2106, 2, 7, 6, 28, 16) == 4294967296ull;

    // Rejected input reports zero rather than a plausible-looking date.
    valid = valid && !rtc64_unix_seconds(1969, 12, 31, 23, 59, 59) &&
        !rtc64_unix_seconds(2026, 0, 1, 0, 0, 0) &&
        !rtc64_unix_seconds(2026, 13, 1, 0, 0, 0) &&
        !rtc64_unix_seconds(2026, 1, 0, 0, 0, 0) &&
        !rtc64_unix_seconds(2026, 1, 32, 0, 0, 0) &&
        !rtc64_unix_seconds(2026, 1, 1, 24, 0, 0) &&
        !rtc64_unix_seconds(2026, 1, 1, 0, 60, 0) &&
        !rtc64_unix_seconds(2026, 1, 1, 0, 0, 61);

    // The anchor has to exist and must not run backwards. Whether the date is
    // sane is deliberately not asserted here: a dead board battery is a real
    // condition the kernel warns about at boot, and it must not take the whole
    // test matrix down with it.
    u64 first = rtc64_wall_clock();
    u64 second = rtc64_wall_clock();
    valid = valid && first && second >= first;

    return valid ? 0 : -1;
}
