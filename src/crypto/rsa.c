#include "rsa.h"
#include "sha256.h"
#include "crypto.h"

typedef unsigned __int128 u128;

#define SALT_SIZE 32u
#define PSS_TRAILER 0xBCu

// DigestInfo prefix for SHA-256 from RFC 8017 section 9.2. The digest follows
// it verbatim, so the whole encoded message is a fixed pattern and can be
// compared byte for byte rather than parsed.
static const u8 sha256_digest_info[19] = {
    0x30, 0x31, 0x30, 0x0D, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20,
};

static u64 montgomery_n0(u64 low_limb) {
    // Newton iteration doubles the correct bits each round, so six rounds
    // cover all 64 starting from a one bit seed.
    u64 inverse = 1;
    for (u32 round = 0; round < 6; round++)
        inverse *= 2ull - low_limb * inverse;
    return 0ull - inverse;
}

static void bn_zero(u64 *out, u32 limbs) {
    for (u32 index = 0; index < limbs; index++) out[index] = 0;
}

static void bn_copy(u64 *out, const u64 *in, u32 limbs) {
    for (u32 index = 0; index < limbs; index++) out[index] = in[index];
}

static u64 bn_sub(u64 *out, const u64 *a, const u64 *b, u32 limbs) {
    u64 borrow = 0;
    for (u32 index = 0; index < limbs; index++) {
        u128 difference = (u128)a[index] - b[index] - borrow;
        out[index] = (u64)difference;
        borrow = (u64)((difference >> 64) & 1u);
    }
    return borrow;
}

static int bn_less(const u64 *a, const u64 *b, u32 limbs) {
    for (u32 index = limbs; index-- > 0;) {
        if (a[index] < b[index]) return 1;
        if (a[index] > b[index]) return 0;
    }
    return 0;
}

static void bn_mont_mul(u64 *out, const u64 *a, const u64 *b, const u64 *m,
                        u32 limbs, u64 n0) {
    u64 accumulator[RSA_MAX_LIMBS + 2];
    for (u32 index = 0; index < limbs + 2; index++) accumulator[index] = 0;
    for (u32 i = 0; i < limbs; i++) {
        u64 carry = 0;
        for (u32 j = 0; j < limbs; j++) {
            u128 sum = (u128)a[i] * b[j] + accumulator[j] + carry;
            accumulator[j] = (u64)sum;
            carry = (u64)(sum >> 64);
        }
        u128 tail = (u128)accumulator[limbs] + carry;
        accumulator[limbs] = (u64)tail;
        accumulator[limbs + 1] = (u64)(tail >> 64);

        u64 factor = accumulator[0] * n0;
        carry = 0;
        for (u32 j = 0; j < limbs; j++) {
            u128 sum = (u128)factor * m[j] + accumulator[j] + carry;
            accumulator[j] = (u64)sum;
            carry = (u64)(sum >> 64);
        }
        tail = (u128)accumulator[limbs] + carry;
        accumulator[limbs] = (u64)tail;
        accumulator[limbs + 1] += (u64)(tail >> 64);

        for (u32 j = 0; j <= limbs; j++) accumulator[j] = accumulator[j + 1];
        accumulator[limbs + 1] = 0;
    }
    u64 reduced[RSA_MAX_LIMBS];
    u64 borrow = bn_sub(reduced, accumulator, m, limbs);
    if (accumulator[limbs] || !borrow)
        bn_copy(out, reduced, limbs);
    else
        bn_copy(out, accumulator, limbs);
}

static void bn_from_bytes(u64 *out, u32 limbs, const u8 *in, u32 length) {
    bn_zero(out, limbs);
    for (u32 index = 0; index < length; index++) {
        u32 position = length - 1u - index;
        out[index / 8u] |= (u64)in[position] << ((index % 8u) * 8u);
    }
}

static void bn_to_bytes(u8 *out, u32 length, const u64 *in) {
    for (u32 index = 0; index < length; index++) {
        u32 position = length - 1u - index;
        out[position] = (u8)(in[index / 8u] >> ((index % 8u) * 8u));
    }
}

int rsa_public_key_init(struct rsa_public_key *key, const u8 *modulus,
                        u32 modulus_bytes, u32 exponent) {
    if (!key || !modulus || !modulus_bytes) return -1;
    if (modulus_bytes > RSA_MAX_MODULUS_BYTES) return -1;
    // A modulus has to be odd for Montgomery arithmetic, and the leading byte
    // has to be non-zero or the caller passed a padded encoding.
    if (!modulus[0] || !(modulus[modulus_bytes - 1u] & 1u)) return -1;
    if (modulus_bytes % 8u) return -1;
    if (exponent < 3u || !(exponent & 1u)) return -1;

    key->limbs = modulus_bytes / 8u;
    key->modulus_bytes = modulus_bytes;
    key->exponent = exponent;
    bn_from_bytes(key->modulus, key->limbs, modulus, modulus_bytes);
    key->n0 = montgomery_n0(key->modulus[0]);

    // R^2 mod n, built by doubling R mod n instead of dividing: start from
    // 2^(64*limbs) mod n expressed as a subtraction, then double 64*limbs
    // times with a conditional subtract each step.
    u64 value[RSA_MAX_LIMBS];
    bn_zero(value, key->limbs);
    value[key->limbs - 1u] = 0x8000000000000000ull;
    u64 reduced[RSA_MAX_LIMBS];
    if (!bn_less(value, key->modulus, key->limbs)) {
        bn_sub(reduced, value, key->modulus, key->limbs);
        bn_copy(value, reduced, key->limbs);
    }
    for (u32 step = 0; step < key->limbs * 64u + 1u; step++) {
        u64 carry = 0;
        for (u32 index = 0; index < key->limbs; index++) {
            u64 shifted = (value[index] << 1) | carry;
            carry = value[index] >> 63;
            value[index] = shifted;
        }
        if (carry || !bn_less(value, key->modulus, key->limbs)) {
            bn_sub(reduced, value, key->modulus, key->limbs);
            bn_copy(value, reduced, key->limbs);
        }
    }
    bn_copy(key->montgomery_r2, value, key->limbs);
    return 0;
}

// Raises the signature to the public exponent. The exponent is public and
// tiny, so a plain square and multiply over its bits is enough.
static int rsa_public_operation(const struct rsa_public_key *key,
                                const u8 *signature, u32 signature_bytes,
                                u8 *encoded) {
    if (signature_bytes != key->modulus_bytes) return -1;
    u64 base[RSA_MAX_LIMBS];
    bn_from_bytes(base, key->limbs, signature, signature_bytes);
    // s must be strictly below n, otherwise it is not a valid representative.
    if (!bn_less(base, key->modulus, key->limbs)) return -1;

    u64 base_mont[RSA_MAX_LIMBS];
    bn_mont_mul(base_mont, base, key->montgomery_r2, key->modulus, key->limbs,
                key->n0);

    u64 result[RSA_MAX_LIMBS];
    u64 one[RSA_MAX_LIMBS];
    bn_zero(one, key->limbs);
    one[0] = 1;
    bn_mont_mul(result, one, key->montgomery_r2, key->modulus, key->limbs,
                key->n0);

    u32 highest = 31u;
    while (highest && !((key->exponent >> highest) & 1u)) highest--;
    for (u32 bit = highest + 1u; bit-- > 0;) {
        bn_mont_mul(result, result, result, key->modulus, key->limbs, key->n0);
        if ((key->exponent >> bit) & 1u)
            bn_mont_mul(result, result, base_mont, key->modulus, key->limbs,
                        key->n0);
    }
    bn_mont_mul(result, result, one, key->modulus, key->limbs, key->n0);
    bn_to_bytes(encoded, key->modulus_bytes, result);
    return 0;
}

int rsa_pkcs1_verify_sha256(const struct rsa_public_key *key,
                            const u8 digest[32], const u8 *signature,
                            u32 signature_bytes) {
    if (!key || !digest || !signature) return -1;
    u8 encoded[RSA_MAX_MODULUS_BYTES];
    if (rsa_public_operation(key, signature, signature_bytes, encoded))
        return -1;

    // Rebuild the expected block and compare, rather than walking the padding
    // and accepting what is found. Parsers that skip forward to the digest are
    // how Bleichenbacher signature forgery gets in.
    u8 expected[RSA_MAX_MODULUS_BYTES];
    u32 length = key->modulus_bytes;
    u32 suffix = sizeof(sha256_digest_info) + SHA256_DIGEST_SIZE;
    if (length < suffix + 11u) return -1;
    expected[0] = 0x00u;
    expected[1] = 0x01u;
    for (u32 index = 2; index < length - suffix - 1u; index++)
        expected[index] = 0xFFu;
    expected[length - suffix - 1u] = 0x00u;
    for (u32 index = 0; index < sizeof(sha256_digest_info); index++)
        expected[length - suffix + index] = sha256_digest_info[index];
    for (u32 index = 0; index < SHA256_DIGEST_SIZE; index++)
        expected[length - SHA256_DIGEST_SIZE + index] = digest[index];

    int matched = crypto_equal(encoded, expected, length);
    crypto_zero(encoded, sizeof(encoded));
    crypto_zero(expected, sizeof(expected));
    return matched ? 0 : -1;
}

static void mgf1_sha256(u8 *out, u32 out_length, const u8 *seed,
                        u32 seed_length) {
    u32 produced = 0;
    u32 counter = 0;
    while (produced < out_length) {
        struct sha256 context;
        u8 counter_bytes[4];
        counter_bytes[0] = (u8)(counter >> 24);
        counter_bytes[1] = (u8)(counter >> 16);
        counter_bytes[2] = (u8)(counter >> 8);
        counter_bytes[3] = (u8)counter;
        u8 block[SHA256_DIGEST_SIZE];
        sha256_init(&context);
        sha256_update(&context, seed, seed_length);
        sha256_update(&context, counter_bytes, sizeof(counter_bytes));
        sha256_final(&context, block);
        u32 remaining = out_length - produced;
        u32 take = remaining < SHA256_DIGEST_SIZE ? remaining
                                                  : SHA256_DIGEST_SIZE;
        for (u32 index = 0; index < take; index++)
            out[produced + index] = block[index];
        produced += take;
        counter++;
        crypto_zero(block, sizeof(block));
    }
}

int rsa_pss_verify_sha256(const struct rsa_public_key *key,
                          const u8 digest[32], const u8 *signature,
                          u32 signature_bytes) {
    if (!key || !digest || !signature) return -1;
    u8 encoded[RSA_MAX_MODULUS_BYTES];
    if (rsa_public_operation(key, signature, signature_bytes, encoded))
        return -1;

    u32 length = key->modulus_bytes;
    if (length < SHA256_DIGEST_SIZE + SALT_SIZE + 2u) return -1;
    if (encoded[length - 1u] != PSS_TRAILER) return -1;

    // emBits is one less than the modulus bit length, so the top bit of the
    // masked block has to be clear.
    if (encoded[0] & 0x80u) return -1;

    u32 db_length = length - SHA256_DIGEST_SIZE - 1u;
    const u8 *masked_db = encoded;
    const u8 *hash = encoded + db_length;

    u8 db_mask[RSA_MAX_MODULUS_BYTES];
    mgf1_sha256(db_mask, db_length, hash, SHA256_DIGEST_SIZE);
    u8 db[RSA_MAX_MODULUS_BYTES];
    for (u32 index = 0; index < db_length; index++)
        db[index] = masked_db[index] ^ db_mask[index];
    db[0] &= 0x7Fu;

    // Everything before the 0x01 separator must be zero, and the salt has to
    // be exactly the length this profile fixes.
    u32 separator = db_length - SALT_SIZE - 1u;
    int well_formed = db[separator] == 0x01u;
    for (u32 index = 0; index < separator; index++)
        well_formed &= db[index] == 0x00u;
    if (!well_formed) {
        crypto_zero(db, sizeof(db));
        return -1;
    }

    u8 prefix[8];
    for (u32 index = 0; index < 8; index++) prefix[index] = 0;
    struct sha256 context;
    u8 recomputed[SHA256_DIGEST_SIZE];
    sha256_init(&context);
    sha256_update(&context, prefix, sizeof(prefix));
    sha256_update(&context, digest, SHA256_DIGEST_SIZE);
    sha256_update(&context, db + separator + 1u, SALT_SIZE);
    sha256_final(&context, recomputed);

    int matched = crypto_equal(recomputed, hash, SHA256_DIGEST_SIZE);
    crypto_zero(db, sizeof(db));
    crypto_zero(db_mask, sizeof(db_mask));
    crypto_zero(encoded, sizeof(encoded));
    crypto_zero(recomputed, sizeof(recomputed));
    return matched ? 0 : -1;
}
