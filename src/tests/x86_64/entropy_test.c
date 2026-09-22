#include "types.h"
#include "entropy.h"

static int bytes_equal(const u8 *left, const u8 *right, u32 length) {
    for (u32 index = 0; index < length; index++)
        if (left[index] != right[index]) return 0;
    return 1;
}

int test_entropy64(void) {
    /* RFC 8439 section 2.4.2 known answer for the ChaCha20 block
       function: key 00..1f, nonce 000000090000004a00000000, counter 1. */
    u8 key[32];
    for (u32 index = 0; index < 32; index++) key[index] = (u8)index;
    u8 nonce[12] = { 0x00, 0x00, 0x00, 0x09, 0x00, 0x00,
                     0x00, 0x4a, 0x00, 0x00, 0x00, 0x00 };
    const u8 expected[64] = {
        0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
        0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
        0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03,
        0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
        0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09,
        0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
        0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9,
        0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e,
    };
    u8 block[64];
    entropy_chacha20_block(key, 1, nonce, block);
    int valid = bytes_equal(block, expected, 64);

    /* Determinism for identical inputs, divergence on counter advance. */
    u8 again[64];
    entropy_chacha20_block(key, 1, nonce, again);
    valid = valid && bytes_equal(block, again, 64);
    u8 advanced[64];
    entropy_chacha20_block(key, 2, nonce, advanced);
    valid = valid && !bytes_equal(block, advanced, 64);

    /* DRBG output: two fills differ, look alive, and guards reject. */
    static u8 first[ENTROPY_FILL_MAX];
    static u8 second[256];
    u32 set_bits = 0;
    int same_zero = 1;
    int same_one = 1;
    valid = valid && !entropy_fill(first, 256) &&
            !entropy_fill(second, 256) &&
            !bytes_equal(first, second, 256);
    for (u32 index = 0; index < 256; index++) {
        if (first[index]) same_zero = 0;
        if (first[index] != 0xFF) same_one = 0;
        for (u32 bit = 0; bit < 8; bit++)
            if (first[index] & (1u << bit)) set_bits++;
    }
    valid = valid && !same_zero && !same_one &&
            set_bits >= 896 && set_bits <= 1152;

    /* The maximum single request must succeed into a full-size buffer. */
    static u8 large[ENTROPY_FILL_MAX];
    valid = valid && !entropy_fill(large, ENTROPY_FILL_MAX) &&
            !bytes_equal(large, first, 256);
    valid = valid && entropy_fill(0, 16) == -1 &&
            entropy_fill(first, 0) == -1 &&
            entropy_fill(first, ENTROPY_FILL_MAX + 1) == -1;
    return valid ? 0 : -1;
}
