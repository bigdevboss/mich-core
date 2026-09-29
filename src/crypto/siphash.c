#include "siphash.h"

static u64 rotl(u64 value, u32 bits) {
    return (value << bits) | (value >> (64 - bits));
}

static u64 load_le64(const u8 *p, u32 length) {
    u64 value = 0;
    for (u32 index = 0; index < length; index++)
        value |= (u64)p[index] << (index * 8);
    return value;
}

#define SIPROUND \
    do { \
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32); \
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2; \
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0; \
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32); \
    } while (0)

u64 siphash_2_4(const u8 key[SIPHASH_KEY_SIZE], const void *data, u32 length) {
    u64 k0 = load_le64(key, 8);
    u64 k1 = load_le64(key + 8, 8);
    u64 v0 = 0x736F6D6570736575ULL ^ k0;
    u64 v1 = 0x646F72616E646F6DULL ^ k1;
    u64 v2 = 0x6C7967656E657261ULL ^ k0;
    u64 v3 = 0x7465646279746573ULL ^ k1;

    const u8 *bytes = (const u8 *)data;
    u32 full = length & ~7u;
    for (u32 offset = 0; offset < full; offset += 8) {
        u64 block = load_le64(bytes + offset, 8);
        v3 ^= block;
        SIPROUND;
        SIPROUND;
        v0 ^= block;
    }

    // The final block always carries the message length in its top byte, so
    // inputs that differ only by trailing zero bytes still hash apart.
    u64 last = (u64)length << 56;
    last |= load_le64(bytes + full, length - full);
    v3 ^= last;
    SIPROUND;
    SIPROUND;
    v0 ^= last;

    v2 ^= 0xFFu;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}
