#include "p256.h"
#include "crypto.h"

// The full 64x64 product is needed by the field arithmetic and by nothing
// outside this file, so the type stays local rather than widening types.h.
typedef unsigned __int128 u128;

#define LIMBS 4u

// Montgomery form throughout: p ends in 2^64-1, so -p^-1 mod 2^64 is 1 and the
// reduction step needs no multiply at all. Solinas reduction would be closer
// to the text of FIPS 186-4 but only reads well on 32-bit limbs.
#define FIELD_N0 1ULL
#define ORDER_N0 0xCCD1C8AAEE00BC4FULL

static const u64 field_p[LIMBS] = {
    0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFFULL,
    0x0000000000000000ULL, 0xFFFFFFFF00000001ULL,
};

static const u64 group_n[LIMBS] = {
    0xF3B9CAC2FC632551ULL, 0xBCE6FAADA7179E84ULL,
    0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFF00000000ULL,
};

static const u64 field_r2[LIMBS] = {
    0x0000000000000003ULL, 0xFFFFFFFBFFFFFFFFULL,
    0xFFFFFFFFFFFFFFFEULL, 0x00000004FFFFFFFDULL,
};

static const u64 group_r2[LIMBS] = {
    0x83244C95BE79EEA2ULL, 0x4699799C49BD6FA6ULL,
    0x2845B2392B6BEC59ULL, 0x66E12D94F3D95620ULL,
};

static const u64 field_one[LIMBS] = {
    0x0000000000000001ULL, 0xFFFFFFFF00000000ULL,
    0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFEULL,
};

static const u64 curve_b[LIMBS] = {
    0xD89CDF6229C4BDDFULL, 0xACF005CD78843090ULL,
    0xE5A220ABF7212ED6ULL, 0xDC30061D04874834ULL,
};

static const u64 base_x[LIMBS] = {
    0x79E730D418A9143CULL, 0x75BA95FC5FEDB601ULL,
    0x79FB732B77622510ULL, 0x18905F76A53755C6ULL,
};

static const u64 base_y[LIMBS] = {
    0xDDF25357CE95560AULL, 0x8B4AB8E4BA19E45CULL,
    0xD2E88688DD21F325ULL, 0x8571FF1825885D85ULL,
};

static void limbs_copy(u64 out[LIMBS], const u64 in[LIMBS]) {
    for (u32 index = 0; index < LIMBS; index++) out[index] = in[index];
}

static void limbs_zero(u64 out[LIMBS]) {
    for (u32 index = 0; index < LIMBS; index++) out[index] = 0;
}

static int limbs_is_zero(const u64 in[LIMBS]) {
    u64 folded = 0;
    for (u32 index = 0; index < LIMBS; index++) folded |= in[index];
    return folded == 0;
}

static int limbs_equal(const u64 a[LIMBS], const u64 b[LIMBS]) {
    for (u32 index = 0; index < LIMBS; index++)
        if (a[index] != b[index]) return 0;
    return 1;
}

// Returns 1 when a < b, reading both as 256-bit little-endian integers.
static int limbs_less(const u64 a[LIMBS], const u64 b[LIMBS]) {
    for (u32 index = LIMBS; index-- > 0;) {
        if (a[index] < b[index]) return 1;
        if (a[index] > b[index]) return 0;
    }
    return 0;
}

static u64 limbs_add(u64 out[LIMBS], const u64 a[LIMBS], const u64 b[LIMBS]) {
    u64 carry = 0;
    for (u32 index = 0; index < LIMBS; index++) {
        u128 sum = (u128)a[index] + b[index] + carry;
        out[index] = (u64)sum;
        carry = (u64)(sum >> 64);
    }
    return carry;
}

static u64 limbs_sub(u64 out[LIMBS], const u64 a[LIMBS], const u64 b[LIMBS]) {
    u64 borrow = 0;
    for (u32 index = 0; index < LIMBS; index++) {
        u128 difference = (u128)a[index] - b[index] - borrow;
        out[index] = (u64)difference;
        borrow = (u64)((difference >> 64) & 1u);
    }
    return borrow;
}

static void mod_add(u64 out[LIMBS], const u64 a[LIMBS], const u64 b[LIMBS],
                    const u64 modulus[LIMBS]) {
    u64 carry = limbs_add(out, a, b);
    u64 reduced[LIMBS];
    u64 borrow = limbs_sub(reduced, out, modulus);
    if (carry || !borrow) limbs_copy(out, reduced);
}

static void mod_sub(u64 out[LIMBS], const u64 a[LIMBS], const u64 b[LIMBS],
                    const u64 modulus[LIMBS]) {
    u64 borrow = limbs_sub(out, a, b);
    if (borrow) {
        u64 wrapped[LIMBS];
        limbs_add(wrapped, out, modulus);
        limbs_copy(out, wrapped);
    }
}

// Coarsely integrated operand scanning: interleaves the multiply with the
// reduction so the intermediate never exceeds five limbs.
static void mont_mul(u64 out[LIMBS], const u64 a[LIMBS], const u64 b[LIMBS],
                     const u64 modulus[LIMBS], u64 n0) {
    u64 accumulator[LIMBS + 2];
    for (u32 index = 0; index < LIMBS + 2; index++) accumulator[index] = 0;
    for (u32 i = 0; i < LIMBS; i++) {
        u64 carry = 0;
        for (u32 j = 0; j < LIMBS; j++) {
            u128 sum = (u128)a[i] * b[j] + accumulator[j] + carry;
            accumulator[j] = (u64)sum;
            carry = (u64)(sum >> 64);
        }
        u128 tail = (u128)accumulator[LIMBS] + carry;
        accumulator[LIMBS] = (u64)tail;
        accumulator[LIMBS + 1] = (u64)(tail >> 64);

        u64 factor = accumulator[0] * n0;
        carry = 0;
        for (u32 j = 0; j < LIMBS; j++) {
            u128 sum = (u128)factor * modulus[j] + accumulator[j] + carry;
            accumulator[j] = (u64)sum;
            carry = (u64)(sum >> 64);
        }
        tail = (u128)accumulator[LIMBS] + carry;
        accumulator[LIMBS] = (u64)tail;
        accumulator[LIMBS + 1] += (u64)(tail >> 64);

        for (u32 j = 0; j <= LIMBS; j++) accumulator[j] = accumulator[j + 1];
        accumulator[LIMBS + 1] = 0;
    }
    u64 reduced[LIMBS];
    u64 borrow = limbs_sub(reduced, accumulator, modulus);
    if (accumulator[LIMBS] || !borrow)
        limbs_copy(out, reduced);
    else
        limbs_copy(out, accumulator);
    crypto_zero(accumulator, sizeof(accumulator));
}

static void fe_mul(u64 out[LIMBS], const u64 a[LIMBS], const u64 b[LIMBS]) {
    mont_mul(out, a, b, field_p, FIELD_N0);
}

static void fe_sqr(u64 out[LIMBS], const u64 a[LIMBS]) {
    mont_mul(out, a, a, field_p, FIELD_N0);
}

static void fe_add(u64 out[LIMBS], const u64 a[LIMBS], const u64 b[LIMBS]) {
    mod_add(out, a, b, field_p);
}

static void fe_sub(u64 out[LIMBS], const u64 a[LIMBS], const u64 b[LIMBS]) {
    mod_sub(out, a, b, field_p);
}

static void fe_to_mont(u64 out[LIMBS], const u64 in[LIMBS]) {
    mont_mul(out, in, field_r2, field_p, FIELD_N0);
}

static void fe_from_mont(u64 out[LIMBS], const u64 in[LIMBS]) {
    u64 one[LIMBS];
    limbs_zero(one);
    one[0] = 1;
    mont_mul(out, in, one, field_p, FIELD_N0);
}

// Inversion as exponentiation by p-2. A hand written addition chain would be
// shorter, but this operation runs twice per verification and nothing secret
// passes through it, so the obvious square and multiply is the safer trade.
static void fe_inv(u64 out[LIMBS], const u64 in[LIMBS]) {
    u64 exponent[LIMBS];
    u64 two[LIMBS];
    limbs_zero(two);
    two[0] = 2;
    limbs_sub(exponent, field_p, two);

    u64 result[LIMBS];
    limbs_copy(result, field_one);
    u64 base[LIMBS];
    limbs_copy(base, in);
    for (u32 bit = 0; bit < 256; bit++) {
        if ((exponent[bit / 64] >> (bit & 63)) & 1u)
            fe_mul(result, result, base);
        fe_sqr(base, base);
    }
    limbs_copy(out, result);
}

static void scalar_mul(u64 out[LIMBS], const u64 a[LIMBS],
                       const u64 b[LIMBS]) {
    mont_mul(out, a, b, group_n, ORDER_N0);
}

static void scalar_to_mont(u64 out[LIMBS], const u64 in[LIMBS]) {
    mont_mul(out, in, group_r2, group_n, ORDER_N0);
}

static void scalar_from_mont(u64 out[LIMBS], const u64 in[LIMBS]) {
    u64 one[LIMBS];
    limbs_zero(one);
    one[0] = 1;
    mont_mul(out, in, one, group_n, ORDER_N0);
}

// n - 2 has no convenient structure, so this is an ordinary square and
// multiply over the bits of the order.
static void scalar_inv(u64 out[LIMBS], const u64 in[LIMBS]) {
    u64 exponent[LIMBS];
    limbs_copy(exponent, group_n);
    u64 two[LIMBS];
    limbs_zero(two);
    two[0] = 2;
    u64 borrow = limbs_sub(exponent, exponent, two);
    (void)borrow;

    u64 result[LIMBS];
    limbs_copy(result, group_r2);
    scalar_from_mont(result, result);
    limbs_zero(result);
    result[0] = 1;
    scalar_to_mont(result, result);

    u64 base[LIMBS];
    limbs_copy(base, in);
    for (u32 bit = 0; bit < 256; bit++) {
        if ((exponent[bit / 64] >> (bit & 63)) & 1u)
            scalar_mul(result, result, base);
        scalar_mul(base, base, base);
    }
    limbs_copy(out, result);
}

// Jacobian coordinates: affine x is X/Z^2 and y is Y/Z^3, which keeps the
// per-step inversion out of the ladder. Z = 0 marks the point at infinity.
struct point {
    u64 x[LIMBS];
    u64 y[LIMBS];
    u64 z[LIMBS];
};

static void point_set_infinity(struct point *out) {
    limbs_copy(out->x, field_one);
    limbs_copy(out->y, field_one);
    limbs_zero(out->z);
}

static int point_is_infinity(const struct point *in) {
    return limbs_is_zero(in->z);
}

static void point_copy(struct point *out, const struct point *in) {
    limbs_copy(out->x, in->x);
    limbs_copy(out->y, in->y);
    limbs_copy(out->z, in->z);
}

// Doubling for a = -3, which lets the usual two squarings collapse into
// (X - Z^2)(X + Z^2).
static void point_double(struct point *out, const struct point *in) {
    if (point_is_infinity(in) || limbs_is_zero(in->y)) {
        point_set_infinity(out);
        return;
    }
    u64 delta[LIMBS];
    u64 gamma[LIMBS];
    u64 beta[LIMBS];
    u64 alpha[LIMBS];
    u64 t0[LIMBS];
    u64 t1[LIMBS];

    fe_sqr(delta, in->z);
    fe_sqr(gamma, in->y);
    fe_mul(beta, in->x, gamma);

    fe_sub(t0, in->x, delta);
    fe_add(t1, in->x, delta);
    fe_mul(alpha, t0, t1);
    fe_add(t0, alpha, alpha);
    fe_add(alpha, t0, alpha);

    fe_add(t0, in->y, in->z);
    fe_sqr(t0, t0);
    fe_sub(t0, t0, gamma);
    fe_sub(t0, t0, delta);
    u64 z_out[LIMBS];
    limbs_copy(z_out, t0);

    fe_sqr(t0, alpha);
    fe_add(t1, beta, beta);
    fe_add(t1, t1, t1);
    fe_add(t1, t1, t1);
    fe_sub(t0, t0, t1);
    u64 x_out[LIMBS];
    limbs_copy(x_out, t0);

    fe_add(t1, beta, beta);
    fe_add(t1, t1, t1);
    fe_sub(t1, t1, x_out);
    fe_mul(t1, alpha, t1);
    fe_sqr(t0, gamma);
    fe_add(t0, t0, t0);
    fe_add(t0, t0, t0);
    fe_add(t0, t0, t0);
    fe_sub(t1, t1, t0);

    limbs_copy(out->x, x_out);
    limbs_copy(out->y, t1);
    limbs_copy(out->z, z_out);
}

static void point_add(struct point *out, const struct point *a,
                      const struct point *b) {
    if (point_is_infinity(a)) {
        point_copy(out, b);
        return;
    }
    if (point_is_infinity(b)) {
        point_copy(out, a);
        return;
    }
    u64 z1z1[LIMBS];
    u64 z2z2[LIMBS];
    u64 u1[LIMBS];
    u64 u2[LIMBS];
    u64 s1[LIMBS];
    u64 s2[LIMBS];
    u64 h[LIMBS];
    u64 i[LIMBS];
    u64 j[LIMBS];
    u64 rr[LIMBS];
    u64 v[LIMBS];
    u64 t0[LIMBS];

    fe_sqr(z1z1, a->z);
    fe_sqr(z2z2, b->z);
    fe_mul(u1, a->x, z2z2);
    fe_mul(u2, b->x, z1z1);
    fe_mul(s1, a->y, b->z);
    fe_mul(s1, s1, z2z2);
    fe_mul(s2, b->y, a->z);
    fe_mul(s2, s2, z1z1);

    if (limbs_equal(u1, u2)) {
        // Same x: either a genuine double or a point plus its negation.
        if (limbs_equal(s1, s2)) point_double(out, a);
        else point_set_infinity(out);
        return;
    }

    fe_sub(h, u2, u1);
    fe_add(i, h, h);
    fe_sqr(i, i);
    fe_mul(j, h, i);
    fe_sub(rr, s2, s1);
    fe_add(rr, rr, rr);
    fe_mul(v, u1, i);

    fe_sqr(t0, rr);
    fe_sub(t0, t0, j);
    fe_sub(t0, t0, v);
    fe_sub(t0, t0, v);
    u64 x_out[LIMBS];
    limbs_copy(x_out, t0);

    fe_sub(t0, v, x_out);
    fe_mul(t0, rr, t0);
    fe_mul(s1, s1, j);
    fe_add(s1, s1, s1);
    fe_sub(t0, t0, s1);
    u64 y_out[LIMBS];
    limbs_copy(y_out, t0);

    fe_add(t0, a->z, b->z);
    fe_sqr(t0, t0);
    fe_sub(t0, t0, z1z1);
    fe_sub(t0, t0, z2z2);
    fe_mul(t0, t0, h);

    limbs_copy(out->x, x_out);
    limbs_copy(out->y, y_out);
    limbs_copy(out->z, t0);
}

static void bytes_to_limbs(u64 out[LIMBS], const u8 in[32]) {
    for (u32 index = 0; index < LIMBS; index++) {
        u64 value = 0;
        for (u32 byte = 0; byte < 8; byte++)
            value = (value << 8) | in[(LIMBS - 1 - index) * 8 + byte];
        out[index] = value;
    }
}

// Confirms y^2 == x^3 - 3x + b. Skipping this lets a peer hand over a point
// from a different, weaker curve.
static int point_on_curve(const u64 x[LIMBS], const u64 y[LIMBS]) {
    u64 left[LIMBS];
    u64 right[LIMBS];
    u64 three_x[LIMBS];
    fe_sqr(left, y);
    fe_sqr(right, x);
    fe_mul(right, right, x);
    fe_add(three_x, x, x);
    fe_add(three_x, three_x, x);
    fe_sub(right, right, three_x);
    fe_add(right, right, curve_b);
    return limbs_equal(left, right);
}

int p256_verify(const u8 public_key[P256_PUBLIC_KEY_SIZE],
                const u8 digest[P256_SCALAR_SIZE],
                const u8 r[P256_SCALAR_SIZE], const u8 s[P256_SCALAR_SIZE]) {
    if (!public_key || !digest || !r || !s) return -1;
    if (public_key[0] != 0x04u) return -1;

    u64 r_value[LIMBS];
    u64 s_value[LIMBS];
    bytes_to_limbs(r_value, r);
    bytes_to_limbs(s_value, s);
    // Both have to sit in [1, n-1]; zero or anything at or above the order is
    // a malformed signature, not a failed one.
    if (limbs_is_zero(r_value) || limbs_is_zero(s_value)) return -1;
    if (!limbs_less(r_value, group_n) || !limbs_less(s_value, group_n))
        return -1;

    u64 qx[LIMBS];
    u64 qy[LIMBS];
    bytes_to_limbs(qx, public_key + 1);
    bytes_to_limbs(qy, public_key + 1 + P256_COORD_SIZE);
    if (!limbs_less(qx, field_p) || !limbs_less(qy, field_p)) return -1;
    u64 qx_mont[LIMBS];
    u64 qy_mont[LIMBS];
    fe_to_mont(qx_mont, qx);
    fe_to_mont(qy_mont, qy);
    if (limbs_is_zero(qx_mont) && limbs_is_zero(qy_mont)) return -1;
    if (!point_on_curve(qx_mont, qy_mont)) return -1;

    u64 e_value[LIMBS];
    bytes_to_limbs(e_value, digest);
    // The digest is reduced, never rejected: SHA-256 output and the order are
    // both 256 bits, so a hash above n is ordinary.
    if (!limbs_less(e_value, group_n)) {
        u64 reduced[LIMBS];
        limbs_sub(reduced, e_value, group_n);
        limbs_copy(e_value, reduced);
    }

    u64 s_mont[LIMBS];
    u64 w_mont[LIMBS];
    scalar_to_mont(s_mont, s_value);
    scalar_inv(w_mont, s_mont);

    u64 e_mont[LIMBS];
    u64 r_mont[LIMBS];
    scalar_to_mont(e_mont, e_value);
    scalar_to_mont(r_mont, r_value);
    u64 u1_mont[LIMBS];
    u64 u2_mont[LIMBS];
    scalar_mul(u1_mont, e_mont, w_mont);
    scalar_mul(u2_mont, r_mont, w_mont);
    u64 u1[LIMBS];
    u64 u2[LIMBS];
    scalar_from_mont(u1, u1_mont);
    scalar_from_mont(u2, u2_mont);

    struct point generator;
    struct point public_point;
    limbs_copy(generator.x, base_x);
    limbs_copy(generator.y, base_y);
    limbs_copy(generator.z, field_one);
    limbs_copy(public_point.x, qx_mont);
    limbs_copy(public_point.y, qy_mont);
    limbs_copy(public_point.z, field_one);

    // Shamir's trick: one pass over the bits of both scalars, picking the
    // precomputed combination instead of running two separate ladders.
    struct point table[4];
    point_set_infinity(&table[0]);
    point_copy(&table[1], &generator);
    point_copy(&table[2], &public_point);
    point_add(&table[3], &generator, &public_point);

    struct point accumulator;
    point_set_infinity(&accumulator);
    for (u32 bit = 256; bit-- > 0;) {
        point_double(&accumulator, &accumulator);
        u32 selector = (u32)(((u1[bit / 64] >> (bit & 63)) & 1u) |
                             (((u2[bit / 64] >> (bit & 63)) & 1u) << 1));
        if (selector) point_add(&accumulator, &accumulator, &table[selector]);
    }

    if (point_is_infinity(&accumulator)) return -1;

    u64 z_inverse[LIMBS];
    u64 z_inverse_squared[LIMBS];
    u64 x_affine[LIMBS];
    fe_inv(z_inverse, accumulator.z);
    fe_sqr(z_inverse_squared, z_inverse);
    fe_mul(x_affine, accumulator.x, z_inverse_squared);
    fe_from_mont(x_affine, x_affine);

    if (!limbs_less(x_affine, group_n)) {
        u64 reduced[LIMBS];
        limbs_sub(reduced, x_affine, group_n);
        limbs_copy(x_affine, reduced);
    }
    return limbs_equal(x_affine, r_value) ? 0 : -1;
}
