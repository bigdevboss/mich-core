#include "test_report.h"
#include "serial64.h"

static struct test_result64 results[TEST_REPORT_MAX];
static u32 count;
static u32 failures;

void test_report_reset(void) {
    count = 0;
    failures = 0;
    for (u32 i = 0; i < TEST_REPORT_MAX; i++) {
        results[i].id = 0;
        results[i].status = 0;
    }
}

int test_report_record(u32 id, int status) {
    if (!id || count >= TEST_REPORT_MAX) return -1;
    results[count].id = id;
    results[count].status = status;
    count++;
    if (status) failures++;
    return status;
}

u32 test_report_count(void) {
    return count;
}

u32 test_report_failures(void) {
    return failures;
}

void test_report_finish(void) {
    serial64_write("Mich test64: result count=0x");
    serial64_hex(count);
    serial64_write(" failures=0x");
    serial64_hex(failures);
    serial64_write("\n");
    u32 value = failures ? 0x11 : 0x10;
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"((u16)0x501));
    if (!failures) {
        struct {
            u16 limit;
            u64 base;
        } __attribute__((packed)) empty_idt = {0, 0};
        __asm__ volatile("lidt %0; int3" : : "m"(empty_idt));
    }
    for (;;) __asm__ volatile("cli; hlt");
}
