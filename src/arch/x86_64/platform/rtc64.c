#include "rtc64.h"
#include "acpi64.h"
#include "serial64.h"

#define RTC_INDEX_PORT 0x70
#define RTC_DATA_PORT 0x71

#define RTC_REGISTER_SECOND 0x00
#define RTC_REGISTER_MINUTE 0x02
#define RTC_REGISTER_HOUR 0x04
#define RTC_REGISTER_DAY 0x07
#define RTC_REGISTER_MONTH 0x08
#define RTC_REGISTER_YEAR 0x09
#define RTC_REGISTER_STATUS_A 0x0A
#define RTC_REGISTER_STATUS_B 0x0B

#define RTC_STATUS_A_UPDATING 0x80
#define RTC_STATUS_B_24_HOUR 0x02
#define RTC_STATUS_B_BINARY 0x04

#define RTC_HOUR_PM 0x80

// FADT stores the index of the century register at this offset, and zero means
// the firmware does not provide one. The register number is not fixed by any
// specification, so it cannot be hardcoded.
#define FADT_CENTURY_OFFSET 108u

#define RTC_READ_ATTEMPTS 16u

extern u32 timer_ticks;

static u64 wall_clock_anchor;
static u32 wall_clock_anchor_tick;

static void rtc_select(u8 index) {
    // Bit 7 of the index port masks NMI. Preserving the caller's NMI state is
    // not worth a read-modify-write here: every access in this file selects a
    // register with bit 7 clear, which leaves NMI enabled.
    __asm__ volatile("outb %0, %1" : : "a"(index), "Nd"((u16)RTC_INDEX_PORT));
}

static u8 rtc_read(u8 index) {
    u8 value;
    rtc_select(index);
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"((u16)RTC_DATA_PORT));
    return value;
}

static int rtc_updating(void) {
    return (rtc_read(RTC_REGISTER_STATUS_A) & RTC_STATUS_A_UPDATING) != 0;
}

static u32 bcd_to_binary(u8 value) {
    return (u32)((value & 0x0Fu) + ((value >> 4) * 10u));
}

struct rtc_reading {
    u32 second;
    u32 minute;
    u32 hour;
    u32 day;
    u32 month;
    u32 year;
    u32 century;
};

static void rtc_sample(struct rtc_reading *out, u8 century_register) {
    out->second = rtc_read(RTC_REGISTER_SECOND);
    out->minute = rtc_read(RTC_REGISTER_MINUTE);
    out->hour = rtc_read(RTC_REGISTER_HOUR);
    out->day = rtc_read(RTC_REGISTER_DAY);
    out->month = rtc_read(RTC_REGISTER_MONTH);
    out->year = rtc_read(RTC_REGISTER_YEAR);
    out->century = century_register ? rtc_read(century_register) : 0;
}

static int readings_equal(const struct rtc_reading *left,
                          const struct rtc_reading *right) {
    return left->second == right->second && left->minute == right->minute &&
        left->hour == right->hour && left->day == right->day &&
        left->month == right->month && left->year == right->year &&
        left->century == right->century;
}

static u8 rtc_century_register(void) {
    const struct acpi_sdt_header *fadt = acpi64_find("FACP");
    if (!fadt || fadt->length <= FADT_CENTURY_OFFSET) return 0;
    return ((const u8 *)fadt)[FADT_CENTURY_OFFSET];
}

// Days since 1970-01-01 for a proleptic Gregorian date, shifting the year so
// that a leap day lands at the end of the internal year and the century rules
// collapse into one division chain.
static u64 days_from_civil(u32 year, u32 month, u32 day) {
    u32 shifted = year - (month <= 2 ? 1u : 0u);
    u32 era = shifted / 400u;
    u32 year_of_era = shifted - era * 400u;
    u32 day_of_year =
        (153u * (month + (month > 2 ? (u32)-3 : 9u)) + 2u) / 5u + day - 1u;
    u32 day_of_era = year_of_era * 365u + year_of_era / 4u -
        year_of_era / 100u + day_of_year;
    return (u64)era * 146097ull + (u64)day_of_era - 719468ull;
}

u64 rtc64_unix_seconds(u32 year, u32 month, u32 day,
                       u32 hour, u32 minute, u32 second) {
    if (year < 1970u || month < 1u || month > 12u || day < 1u || day > 31u ||
        hour > 23u || minute > 59u || second > 60u)
        return 0;
    u64 days = days_from_civil(year, month, day);
    return days * 86400ull + (u64)hour * 3600ull + (u64)minute * 60ull +
        (u64)second;
}

int rtc64_init(void) {
    u8 century_register = rtc_century_register();
    struct rtc_reading current;
    struct rtc_reading previous;
    u32 attempt = 0;
    // The chip rolls its registers over once a second, and a read that lands
    // inside that window returns a mix of old and new fields. Sampling twice
    // and requiring agreement is stricter than waiting on the update flag,
    // which can itself be observed mid-flip.
    for (; attempt < RTC_READ_ATTEMPTS; attempt++) {
        while (rtc_updating()) { }
        rtc_sample(&previous, century_register);
        while (rtc_updating()) { }
        rtc_sample(&current, century_register);
        if (readings_equal(&previous, &current)) break;
    }
    if (attempt == RTC_READ_ATTEMPTS) return -1;

    u8 status_b = rtc_read(RTC_REGISTER_STATUS_B);
    u32 pm = 0;
    if (!(status_b & RTC_STATUS_B_24_HOUR)) {
        pm = (current.hour & RTC_HOUR_PM) != 0;
        current.hour &= (u32)~RTC_HOUR_PM;
    }
    if (!(status_b & RTC_STATUS_B_BINARY)) {
        current.second = bcd_to_binary((u8)current.second);
        current.minute = bcd_to_binary((u8)current.minute);
        current.hour = bcd_to_binary((u8)current.hour);
        current.day = bcd_to_binary((u8)current.day);
        current.month = bcd_to_binary((u8)current.month);
        current.year = bcd_to_binary((u8)current.year);
        current.century = bcd_to_binary((u8)current.century);
    }
    if (pm) {
        // 12 PM is noon and 12 AM is midnight, so the hour does not simply
        // gain twelve in both halves of the day.
        current.hour = current.hour == 12u ? 12u : current.hour + 12u;
    } else if (!(status_b & RTC_STATUS_B_24_HOUR) && current.hour == 12u) {
        current.hour = 0;
    }

    u32 year = current.century >= 19u ?
        current.century * 100u + current.year : 2000u + current.year;
    u64 seconds = rtc64_unix_seconds(year, current.month, current.day,
                                     current.hour, current.minute,
                                     current.second);
    if (!seconds) return -1;

    wall_clock_anchor = seconds;
    wall_clock_anchor_tick = timer_ticks;

    // The clock is trusted as read. Skipping certificate date checks on a
    // suspect clock would accept revoked and expired certificates, so the
    // failure has to stay visible instead of being worked around.
    if (seconds < RTC64_SANE_EPOCH) {
        serial64_write("Mich x86_64: RTC reports a date before the release "
                       "baseline\n");
        serial64_write("Mich x86_64: board battery is likely dead, "
                       "certificate dates will be rejected\n");
    }
    return 0;
}

u64 rtc64_wall_clock(void) {
    if (!wall_clock_anchor) return 0;
    u32 elapsed = timer_ticks - wall_clock_anchor_tick;
    return wall_clock_anchor + elapsed / RTC64_TICK_HZ;
}
