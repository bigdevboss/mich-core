#include "crc32c.h"

#define CPUID_ECX_SSE42 (1u << 20)

int crc32c_hardware_available(void) {
    // The answer is fixed for the life of the machine, so cache it: otherwise
    // this cpuid runs once per checksummed block. Single CPU, so the static
    // holds.
    static int cached = -1;
    if (cached < 0) {
        u32 eax, ebx, ecx, edx;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(1u), "c"(0u));
        cached = (ecx & CPUID_ECX_SSE42) ? 1 : 0;
    }
    return cached;
}

// The x86-64 crc32 instruction and the bitwise fallback share the one reflected
// Castagnoli polynomial, so both paths return the same value; the test pins that
// with the standard check vector.
static u32 crc32c_hardware(const u8 *data, u32 length) {
    u32 crc = 0xFFFFFFFFu;
    for (u32 index = 0; index < length; index++)
        __asm__("crc32b %b1, %0" : "+r"(crc) : "r"((u32)data[index]));
    return crc ^ 0xFFFFFFFFu;
}

static u32 crc32c_software(const u8 *data, u32 length) {
    u32 crc = 0xFFFFFFFFu;
    for (u32 index = 0; index < length; index++) {
        crc ^= data[index];
        for (u32 bit = 0; bit < 8; bit++)
            crc = (crc & 1u) ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
    }
    return crc ^ 0xFFFFFFFFu;
}

u32 crc32c(const void *data, u32 length) {
    const u8 *bytes = (const u8 *)data;
    if (!bytes) return 0;
    return crc32c_hardware_available() ? crc32c_hardware(bytes, length)
                                       : crc32c_software(bytes, length);
}
