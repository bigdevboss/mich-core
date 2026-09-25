#include "x25519.h"
#include "crypto.h"

// Field arithmetic needs the full 64x64 product, and nothing outside this file
// does. The type stays local so the project-wide aliases in types.h are not
// widened for one algorithm.
typedef unsigned __int128 u128;

#define LIMB_MASK 0x7FFFFFFFFFFFFull
#define LIMB_BITS 51u

// Five limbs of 51 bits instead of four of 64: the representation is
// redundant, so carries can be deferred to the end of a multiplication rather
// than propagated after every step.
struct fe {
    u64 limb[5];
};

static void fe_zero(struct fe *out) {
    for (u32 index = 0; index < 5; index++) out->limb[index] = 0;
}

static void fe_one(struct fe *out) {
    fe_zero(out);
    out->limb[0] = 1;
}

static void fe_copy(struct fe *out, const struct fe *in) {
    for (u32 index = 0; index < 5; index++) out->limb[index] = in->limb[index];
}

static void fe_add(struct fe *out, const struct fe *a, const struct fe *b) {
    for (u32 index = 0; index < 5; index++)
        out->limb[index] = a->limb[index] + b->limb[index];
}

static void fe_sub(struct fe *out, const struct fe *a, const struct fe *b) {
    // Adding 2p before subtracting keeps every limb non-negative without a
    // branch. The low limb carries the 19 that 2^255 = 19 (mod p) folds back.
    out->limb[0] = a->limb[0] + 0xFFFFFFFFFFFDAull - b->limb[0];
    for (u32 index = 1; index < 5; index++)
        out->limb[index] = a->limb[index] + 0xFFFFFFFFFFFFEull - b->limb[index];
}

static void fe_carry(struct fe *out, u128 product[5]) {
    u64 carry;
    out->limb[0] = (u64)product[0] & LIMB_MASK;
    carry = (u64)(product[0] >> LIMB_BITS);
    product[1] += carry;
    out->limb[1] = (u64)product[1] & LIMB_MASK;
    carry = (u64)(product[1] >> LIMB_BITS);
    product[2] += carry;
    out->limb[2] = (u64)product[2] & LIMB_MASK;
    carry = (u64)(product[2] >> LIMB_BITS);
    product[3] += carry;
    out->limb[3] = (u64)product[3] & LIMB_MASK;
    carry = (u64)(product[3] >> LIMB_BITS);
    product[4] += carry;
    out->limb[4] = (u64)product[4] & LIMB_MASK;
    carry = (u64)(product[4] >> LIMB_BITS);
    // Everything above bit 255 comes back multiplied by 19.
    out->limb[0] += carry * 19u;
    carry = out->limb[0] >> LIMB_BITS;
    out->limb[0] &= LIMB_MASK;
    out->limb[1] += carry;
    carry = out->limb[1] >> LIMB_BITS;
    out->limb[1] &= LIMB_MASK;
    out->limb[2] += carry;
}

static void fe_mul(struct fe *out, const struct fe *a, const struct fe *b) {
    u64 a0 = a->limb[0];
    u64 a1 = a->limb[1];
    u64 a2 = a->limb[2];
    u64 a3 = a->limb[3];
    u64 a4 = a->limb[4];
    u64 b0 = b->limb[0];
    u64 b1 = b->limb[1];
    u64 b2 = b->limb[2];
    u64 b3 = b->limb[3];
    u64 b4 = b->limb[4];
    u128 product[5];
    product[0] = (u128)a0 * b0;
    product[1] = (u128)a0 * b1 + (u128)a1 * b0;
    product[2] = (u128)a0 * b2 + (u128)a2 * b0 + (u128)a1 * b1;
    product[3] = (u128)a0 * b3 + (u128)a3 * b0 + (u128)a1 * b2 +
        (u128)a2 * b1;
    product[4] = (u128)a0 * b4 + (u128)a4 * b0 + (u128)a1 * b3 +
        (u128)a3 * b1 + (u128)a2 * b2;
    u64 b1x = b1 * 19u;
    u64 b2x = b2 * 19u;
    u64 b3x = b3 * 19u;
    u64 b4x = b4 * 19u;
    product[0] += (u128)a1 * b4x + (u128)a4 * b1x + (u128)a2 * b3x +
        (u128)a3 * b2x;
    product[1] += (u128)a2 * b4x + (u128)a4 * b2x + (u128)a3 * b3x;
    product[2] += (u128)a3 * b4x + (u128)a4 * b3x;
    product[3] += (u128)a4 * b4x;
    fe_carry(out, product);
}

static void fe_square(struct fe *out, const struct fe *a) {
    fe_mul(out, a, a);
}

static void fe_mul121666(struct fe *out, const struct fe *a) {
    u128 product[5];
    for (u32 index = 0; index < 5; index++)
        product[index] = (u128)a->limb[index] * 121666ull;
    fe_carry(out, product);
}

// Swaps the two elements when selector is 1, in the same time and with the
// same memory traffic as when it is 0. A branch here would leak the scalar
// bit that drives the ladder.
static void fe_cswap(struct fe *a, struct fe *b, u64 selector) {
    u64 mask = 0ull - selector;
    for (u32 index = 0; index < 5; index++) {
        u64 difference = mask & (a->limb[index] ^ b->limb[index]);
        a->limb[index] ^= difference;
        b->limb[index] ^= difference;
    }
}

static void fe_from_bytes(struct fe *out, const u8 in[32]) {
    u64 words[4];
    for (u32 index = 0; index < 4; index++) {
        u64 value = 0;
        for (u32 byte = 0; byte < 8; byte++)
            value |= (u64)in[index * 8 + byte] << (byte * 8);
        words[index] = value;
    }
    out->limb[0] = words[0] & LIMB_MASK;
    out->limb[1] = ((words[0] >> 51) | (words[1] << 13)) & LIMB_MASK;
    out->limb[2] = ((words[1] >> 38) | (words[2] << 26)) & LIMB_MASK;
    out->limb[3] = ((words[2] >> 25) | (words[3] << 39)) & LIMB_MASK;
    // RFC 7748 requires the top bit of the u-coordinate to be ignored rather
    // than treated as an error.
    out->limb[4] = (words[3] >> 12) & LIMB_MASK;
}

static void fe_to_bytes(u8 out[32], const struct fe *in) {
    struct fe value;
    fe_copy(&value, in);

    u64 carry = value.limb[0] >> LIMB_BITS;
    value.limb[0] &= LIMB_MASK;
    for (u32 index = 1; index < 5; index++) {
        value.limb[index] += carry;
        carry = value.limb[index] >> LIMB_BITS;
        value.limb[index] &= LIMB_MASK;
    }
    value.limb[0] += carry * 19u;
    carry = value.limb[0] >> LIMB_BITS;
    value.limb[0] &= LIMB_MASK;
    value.limb[1] += carry;

    // The representation is still one of two: v or v + p. Add 19 and look at
    // the bit that overflows to decide, then subtract p when it is set.
    u64 probe = value.limb[0] + 19u;
    carry = probe >> LIMB_BITS;
    for (u32 index = 1; index < 4; index++)
        carry = (value.limb[index] + carry) >> LIMB_BITS;
    carry = (value.limb[4] + carry) >> LIMB_BITS;

    value.limb[0] += 19u * carry;
    carry = value.limb[0] >> LIMB_BITS;
    value.limb[0] &= LIMB_MASK;
    for (u32 index = 1; index < 5; index++) {
        value.limb[index] += carry;
        carry = value.limb[index] >> LIMB_BITS;
        value.limb[index] &= LIMB_MASK;
    }

    u64 words[4];
    words[0] = value.limb[0] | (value.limb[1] << 51);
    words[1] = (value.limb[1] >> 13) | (value.limb[2] << 38);
    words[2] = (value.limb[2] >> 26) | (value.limb[3] << 25);
    words[3] = (value.limb[3] >> 39) | (value.limb[4] << 12);
    for (u32 index = 0; index < 4; index++)
        for (u32 byte = 0; byte < 8; byte++)
            out[index * 8 + byte] = (u8)(words[index] >> (byte * 8));
    crypto_zero(&value, sizeof(value));
}

// Inversion as exponentiation by p-2, because the addition chain runs the same
// sequence of squarings and multiplications for every input. A binary GCD
// would branch on the value being inverted.
static void fe_invert(struct fe *out, const struct fe *in) {
    struct fe z2;
    struct fe z9;
    struct fe z11;
    struct fe z2_5_0;
    struct fe z2_10_0;
    struct fe z2_20_0;
    struct fe z2_50_0;
    struct fe z2_100_0;
    struct fe t0;
    struct fe t1;

    fe_square(&z2, in);
    fe_square(&t1, &z2);
    fe_square(&t0, &t1);
    fe_mul(&z9, &t0, in);
    fe_mul(&z11, &z9, &z2);
    fe_square(&t0, &z11);
    fe_mul(&z2_5_0, &t0, &z9);

    fe_square(&t0, &z2_5_0);
    for (u32 index = 1; index < 5; index++) fe_square(&t0, &t0);
    fe_mul(&z2_10_0, &t0, &z2_5_0);

    fe_square(&t0, &z2_10_0);
    for (u32 index = 1; index < 10; index++) fe_square(&t0, &t0);
    fe_mul(&z2_20_0, &t0, &z2_10_0);

    fe_square(&t0, &z2_20_0);
    for (u32 index = 1; index < 20; index++) fe_square(&t0, &t0);
    fe_mul(&t0, &t0, &z2_20_0);

    for (u32 index = 0; index < 10; index++) fe_square(&t0, &t0);
    fe_mul(&z2_50_0, &t0, &z2_10_0);

    fe_square(&t0, &z2_50_0);
    for (u32 index = 1; index < 50; index++) fe_square(&t0, &t0);
    fe_mul(&z2_100_0, &t0, &z2_50_0);

    fe_square(&t0, &z2_100_0);
    for (u32 index = 1; index < 100; index++) fe_square(&t0, &t0);
    fe_mul(&t0, &t0, &z2_100_0);

    for (u32 index = 0; index < 50; index++) fe_square(&t0, &t0);
    fe_mul(&t0, &t0, &z2_50_0);

    for (u32 index = 0; index < 5; index++) fe_square(&t0, &t0);
    fe_mul(out, &t0, &z11);

    crypto_zero(&z2, sizeof(z2));
    crypto_zero(&z9, sizeof(z9));
    crypto_zero(&z11, sizeof(z11));
    crypto_zero(&z2_5_0, sizeof(z2_5_0));
    crypto_zero(&z2_10_0, sizeof(z2_10_0));
    crypto_zero(&z2_20_0, sizeof(z2_20_0));
    crypto_zero(&z2_50_0, sizeof(z2_50_0));
    crypto_zero(&z2_100_0, sizeof(z2_100_0));
    crypto_zero(&t0, sizeof(t0));
    crypto_zero(&t1, sizeof(t1));
}

int x25519(u8 out[X25519_KEY_SIZE], const u8 scalar[X25519_KEY_SIZE],
           const u8 point[X25519_KEY_SIZE]) {
    if (!out || !scalar || !point) return -1;

    u8 clamped[X25519_KEY_SIZE];
    for (u32 index = 0; index < X25519_KEY_SIZE; index++)
        clamped[index] = scalar[index];
    // RFC 7748 clamping: clearing the low bits keeps the scalar in the prime
    // order subgroup, and forcing bit 254 fixes the ladder length.
    clamped[0] &= 248u;
    clamped[31] &= 127u;
    clamped[31] |= 64u;

    struct fe base;
    fe_from_bytes(&base, point);

    struct fe x1;
    struct fe x2;
    struct fe z2;
    struct fe x3;
    struct fe z3;
    fe_copy(&x1, &base);
    fe_one(&x2);
    fe_zero(&z2);
    fe_copy(&x3, &base);
    fe_one(&z3);

    struct fe a;
    struct fe b;
    struct fe c;
    struct fe d;
    struct fe e;
    struct fe aa;
    struct fe bb;
    struct fe da;
    struct fe cb;
    u64 swap = 0;
    for (u32 position = 255; position-- > 0;) {
        u64 bit = (clamped[position / 8] >> (position & 7)) & 1u;
        swap ^= bit;
        fe_cswap(&x2, &x3, swap);
        fe_cswap(&z2, &z3, swap);
        swap = bit;

        fe_sub(&a, &x2, &z2);
        fe_square(&aa, &a);
        fe_add(&b, &x2, &z2);
        fe_square(&bb, &b);
        fe_sub(&e, &bb, &aa);
        fe_sub(&c, &x3, &z3);
        fe_add(&d, &x3, &z3);
        fe_mul(&da, &d, &a);
        fe_mul(&cb, &c, &b);
        fe_add(&x3, &da, &cb);
        fe_square(&x3, &x3);
        fe_sub(&z3, &da, &cb);
        fe_square(&z3, &z3);
        fe_mul(&z3, &z3, &x1);
        fe_mul(&x2, &aa, &bb);
        fe_mul121666(&z2, &e);
        fe_add(&z2, &z2, &aa);
        fe_mul(&z2, &z2, &e);
    }
    fe_cswap(&x2, &x3, swap);
    fe_cswap(&z2, &z3, swap);

    fe_invert(&z2, &z2);
    fe_mul(&x2, &x2, &z2);
    fe_to_bytes(out, &x2);

    crypto_zero(clamped, sizeof(clamped));
    crypto_zero(&x1, sizeof(x1));
    crypto_zero(&x2, sizeof(x2));
    crypto_zero(&z2, sizeof(z2));
    crypto_zero(&x3, sizeof(x3));
    crypto_zero(&z3, sizeof(z3));
    crypto_zero(&a, sizeof(a));
    crypto_zero(&b, sizeof(b));
    crypto_zero(&c, sizeof(c));
    crypto_zero(&d, sizeof(d));
    crypto_zero(&e, sizeof(e));
    crypto_zero(&aa, sizeof(aa));
    crypto_zero(&bb, sizeof(bb));
    crypto_zero(&da, sizeof(da));
    crypto_zero(&cb, sizeof(cb));

    u8 zero[X25519_KEY_SIZE];
    for (u32 index = 0; index < X25519_KEY_SIZE; index++) zero[index] = 0;
    // A small-order peer point collapses the shared secret to zero. TLS 1.3
    // must abort on that, so it is reported as a failure here rather than
    // returned as a usable key.
    if (crypto_equal(out, zero, X25519_KEY_SIZE)) return -1;
    return 0;
}

int x25519_base(u8 out[X25519_KEY_SIZE], const u8 scalar[X25519_KEY_SIZE]) {
    u8 base[X25519_KEY_SIZE];
    for (u32 index = 0; index < X25519_KEY_SIZE; index++) base[index] = 0;
    base[0] = 9;
    return x25519(out, scalar, base);
}
