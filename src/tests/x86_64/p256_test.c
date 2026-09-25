#include "types.h"
#include "p256.h"
#include "crypto.h"
#include "tests64.h"

// FIPS 186-4 signature verification, P-256 with SHA-256. Verification handles
// only public values, so the interesting cases are the malformed ones: the
// standard's own test set supplies signatures that must be refused, and the
// rest of the rejections below cover inputs a hostile peer controls.

static const u8 key_x[32] = {
    0xE4, 0x24, 0xDC, 0x61, 0xD4, 0xBB, 0x3C, 0xB7, 0xEF, 0x43, 0x44, 0xA7,
    0xF8, 0x95, 0x7A, 0x0C, 0x51, 0x34, 0xE1, 0x6F, 0x7A, 0x67, 0xC0, 0x74,
    0xF8, 0x2E, 0x6E, 0x12, 0xF4, 0x9A, 0xBF, 0x3C,
};
static const u8 key_y[32] = {
    0x97, 0x0E, 0xED, 0x7A, 0xA2, 0xBC, 0x48, 0x65, 0x15, 0x45, 0x94, 0x9D,
    0xE1, 0xDD, 0xDA, 0xF0, 0x12, 0x7E, 0x59, 0x65, 0xAC, 0x85, 0xD1, 0x24,
    0x3D, 0x6F, 0x60, 0xE7, 0xDF, 0xAE, 0xE9, 0x27,
};
static const u8 digest_valid[32] = {
    0xD1, 0xB8, 0xEF, 0x21, 0xEB, 0x41, 0x82, 0xEE, 0x27, 0x06, 0x38, 0x06,
    0x10, 0x63, 0xA3, 0xF3, 0xC1, 0x6C, 0x11, 0x4E, 0x33, 0x93, 0x7F, 0x69,
    0xFB, 0x23, 0x2C, 0xC8, 0x33, 0x96, 0x5A, 0x94,
};
static const u8 sig_r[32] = {
    0xBF, 0x96, 0xB9, 0x9A, 0xA4, 0x9C, 0x70, 0x5C, 0x91, 0x0B, 0xE3, 0x31,
    0x42, 0x01, 0x7C, 0x64, 0x2F, 0xF5, 0x40, 0xC7, 0x63, 0x49, 0xB9, 0xDA,
    0xB7, 0x2F, 0x98, 0x1F, 0xD9, 0x34, 0x7F, 0x4F,
};
static const u8 sig_s[32] = {
    0x17, 0xC5, 0x50, 0x95, 0x81, 0x90, 0x89, 0xC2, 0xE0, 0x3B, 0x9C, 0xD4,
    0x15, 0xAB, 0xDF, 0x12, 0x44, 0x4E, 0x32, 0x30, 0x75, 0xD9, 0x8F, 0x31,
    0x92, 0x0B, 0x9E, 0x0F, 0x57, 0xEC, 0x87, 0x1C,
};
static const u8 order_n[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84,
    0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51,
};
static const u8 field_p_bytes[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static void build_key(u8 key[P256_PUBLIC_KEY_SIZE], const u8 x[32],
                      const u8 y[32]) {
    key[0] = 0x04u;
    for (u32 index = 0; index < 32; index++) {
        key[1 + index] = x[index];
        key[33 + index] = y[index];
    }
}

int test_p256_64(void) {
    u8 key[P256_PUBLIC_KEY_SIZE];
    build_key(key, key_x, key_y);
    int valid = !p256_verify(key, digest_valid, sig_r, sig_s);

    // One flipped bit anywhere in the signed material has to break it.
    u8 broken[32];
    for (u32 index = 0; index < 32; index++) broken[index] = digest_valid[index];
    broken[31] ^= 1u;
    valid = valid && p256_verify(key, broken, sig_r, sig_s) == -1;
    broken[31] ^= 1u;
    broken[0] ^= 0x80u;
    valid = valid && p256_verify(key, broken, sig_r, sig_s) == -1;

    for (u32 index = 0; index < 32; index++) broken[index] = sig_r[index];
    broken[31] ^= 1u;
    valid = valid && p256_verify(key, digest_valid, broken, sig_s) == -1;

    for (u32 index = 0; index < 32; index++) broken[index] = sig_s[index];
    broken[31] ^= 1u;
    valid = valid && p256_verify(key, digest_valid, sig_r, broken) == -1;

    // r and s must land in [1, n-1]. Zero and the order itself are malformed
    // rather than merely wrong.
    u8 zero[32];
    for (u32 index = 0; index < 32; index++) zero[index] = 0;
    valid = valid && p256_verify(key, digest_valid, zero, sig_s) == -1 &&
        p256_verify(key, digest_valid, sig_r, zero) == -1 &&
        p256_verify(key, digest_valid, order_n, sig_s) == -1 &&
        p256_verify(key, digest_valid, sig_r, order_n) == -1;

    // A public key off the curve opens the invalid-curve attack, so it is
    // refused before any scalar multiplication happens.
    u8 off_curve[P256_PUBLIC_KEY_SIZE];
    build_key(off_curve, key_x, key_y);
    off_curve[P256_PUBLIC_KEY_SIZE - 1] ^= 1u;
    valid = valid && p256_verify(off_curve, digest_valid, sig_r, sig_s) == -1;

    u8 zero_key[P256_PUBLIC_KEY_SIZE];
    build_key(zero_key, zero, zero);
    valid = valid && p256_verify(zero_key, digest_valid, sig_r, sig_s) == -1;

    // Coordinates at or above the field prime are not valid encodings.
    u8 huge_key[P256_PUBLIC_KEY_SIZE];
    build_key(huge_key, field_p_bytes, key_y);
    valid = valid && p256_verify(huge_key, digest_valid, sig_r, sig_s) == -1;

    // Only the uncompressed form is accepted; a compressed point would need
    // a square root that this code does not implement.
    u8 compressed[P256_PUBLIC_KEY_SIZE];
    build_key(compressed, key_x, key_y);
    compressed[0] = 0x02u;
    valid = valid && p256_verify(compressed, digest_valid, sig_r, sig_s) == -1;

    valid = valid && p256_verify(0, digest_valid, sig_r, sig_s) == -1 &&
        p256_verify(key, 0, sig_r, sig_s) == -1;

    // Still valid after all the rejections: no shared state was corrupted.
    valid = valid && !p256_verify(key, digest_valid, sig_r, sig_s);
    return valid ? 0 : -1;
}
