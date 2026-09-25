#include "sha256.h"
#include "crypto.h"

#define HMAC_IPAD 0x36u
#define HMAC_OPAD 0x5Cu

static const u32 round_constants[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u,
    0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
    0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u,
    0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u,
    0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

static u32 rotate_right(u32 value, u32 bits) {
    return (value >> bits) | (value << (32u - bits));
}

static u32 load_be32(const u8 *source) {
    return ((u32)source[0] << 24) | ((u32)source[1] << 16) |
        ((u32)source[2] << 8) | (u32)source[3];
}

static void store_be32(u8 *destination, u32 value) {
    destination[0] = (u8)(value >> 24);
    destination[1] = (u8)(value >> 16);
    destination[2] = (u8)(value >> 8);
    destination[3] = (u8)value;
}

static void sha256_compress(u32 state[8], const u8 block[SHA256_BLOCK_SIZE]) {
    u32 schedule[64];
    for (u32 index = 0; index < 16; index++)
        schedule[index] = load_be32(block + index * 4);
    for (u32 index = 16; index < 64; index++) {
        u32 previous = schedule[index - 15];
        u32 recent = schedule[index - 2];
        u32 s0 = rotate_right(previous, 7) ^ rotate_right(previous, 18) ^
            (previous >> 3);
        u32 s1 = rotate_right(recent, 17) ^ rotate_right(recent, 19) ^
            (recent >> 10);
        schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }
    u32 a = state[0];
    u32 b = state[1];
    u32 c = state[2];
    u32 d = state[3];
    u32 e = state[4];
    u32 f = state[5];
    u32 g = state[6];
    u32 h = state[7];
    for (u32 index = 0; index < 64; index++) {
        u32 s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        u32 choice = (e & f) ^ ((~e) & g);
        u32 temp1 = h + s1 + choice + round_constants[index] + schedule[index];
        u32 s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        u32 majority = (a & b) ^ (a & c) ^ (b & c);
        u32 temp2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
    crypto_zero(schedule, sizeof(schedule));
}

void sha256_init(struct sha256 *context) {
    if (!context) return;
    context->state[0] = 0x6A09E667u;
    context->state[1] = 0xBB67AE85u;
    context->state[2] = 0x3C6EF372u;
    context->state[3] = 0xA54FF53Au;
    context->state[4] = 0x510E527Fu;
    context->state[5] = 0x9B05688Cu;
    context->state[6] = 0x1F83D9ABu;
    context->state[7] = 0x5BE0CD19u;
    context->length = 0;
    context->buffered = 0;
    for (u32 index = 0; index < SHA256_BLOCK_SIZE; index++)
        context->block[index] = 0;
}

void sha256_update(struct sha256 *context, const void *data, u32 length) {
    if (!context || (!data && length)) return;
    const u8 *bytes = (const u8 *)data;
    context->length += (u64)length;
    while (length) {
        u32 space = SHA256_BLOCK_SIZE - context->buffered;
        u32 take = length < space ? length : space;
        for (u32 index = 0; index < take; index++)
            context->block[context->buffered + index] = bytes[index];
        context->buffered += take;
        bytes += take;
        length -= take;
        if (context->buffered == SHA256_BLOCK_SIZE) {
            sha256_compress(context->state, context->block);
            context->buffered = 0;
        }
    }
}

void sha256_final(struct sha256 *context, u8 digest[SHA256_DIGEST_SIZE]) {
    if (!context || !digest) return;
    u64 bits = context->length * 8ull;
    context->block[context->buffered++] = 0x80u;
    // The length occupies the last eight bytes, so a block with no room for it
    // is padded out and compressed before the final block is built.
    if (context->buffered > SHA256_BLOCK_SIZE - 8u) {
        while (context->buffered < SHA256_BLOCK_SIZE)
            context->block[context->buffered++] = 0;
        sha256_compress(context->state, context->block);
        context->buffered = 0;
    }
    while (context->buffered < SHA256_BLOCK_SIZE - 8u)
        context->block[context->buffered++] = 0;
    for (u32 index = 0; index < 8; index++)
        context->block[SHA256_BLOCK_SIZE - 1u - index] = (u8)(bits >> (index * 8));
    sha256_compress(context->state, context->block);
    for (u32 index = 0; index < 8; index++)
        store_be32(digest + index * 4, context->state[index]);
    crypto_zero(context->block, SHA256_BLOCK_SIZE);
}

void sha256_digest(const void *data, u32 length,
                   u8 digest[SHA256_DIGEST_SIZE]) {
    struct sha256 context;
    sha256_init(&context);
    sha256_update(&context, data, length);
    sha256_final(&context, digest);
    crypto_zero(&context, sizeof(context));
}

void hmac_sha256_init(struct hmac_sha256 *context, const void *key,
                      u32 key_length) {
    if (!context || (!key && key_length)) return;
    u8 padded[SHA256_BLOCK_SIZE];
    for (u32 index = 0; index < SHA256_BLOCK_SIZE; index++) padded[index] = 0;
    if (key_length > SHA256_BLOCK_SIZE) {
        sha256_digest(key, key_length, padded);
    } else {
        const u8 *bytes = (const u8 *)key;
        for (u32 index = 0; index < key_length; index++)
            padded[index] = bytes[index];
    }
    u8 scratch[SHA256_BLOCK_SIZE];
    for (u32 index = 0; index < SHA256_BLOCK_SIZE; index++)
        scratch[index] = padded[index] ^ HMAC_IPAD;
    sha256_init(&context->inner);
    sha256_update(&context->inner, scratch, SHA256_BLOCK_SIZE);
    for (u32 index = 0; index < SHA256_BLOCK_SIZE; index++)
        scratch[index] = padded[index] ^ HMAC_OPAD;
    sha256_init(&context->outer);
    sha256_update(&context->outer, scratch, SHA256_BLOCK_SIZE);
    crypto_zero(scratch, sizeof(scratch));
    crypto_zero(padded, sizeof(padded));
}

void hmac_sha256_update(struct hmac_sha256 *context, const void *data,
                        u32 length) {
    if (!context) return;
    sha256_update(&context->inner, data, length);
}

void hmac_sha256_final(struct hmac_sha256 *context,
                       u8 mac[SHA256_DIGEST_SIZE]) {
    if (!context || !mac) return;
    u8 inner[SHA256_DIGEST_SIZE];
    sha256_final(&context->inner, inner);
    sha256_update(&context->outer, inner, SHA256_DIGEST_SIZE);
    sha256_final(&context->outer, mac);
    crypto_zero(inner, sizeof(inner));
}

void hmac_sha256_digest(const void *key, u32 key_length, const void *data,
                        u32 length, u8 mac[SHA256_DIGEST_SIZE]) {
    struct hmac_sha256 context;
    hmac_sha256_init(&context, key, key_length);
    hmac_sha256_update(&context, data, length);
    hmac_sha256_final(&context, mac);
    crypto_zero(&context, sizeof(context));
}

void hkdf_sha256_extract(const void *salt, u32 salt_length, const void *key,
                         u32 key_length, u8 prk[SHA256_DIGEST_SIZE]) {
    if (!prk) return;
    u8 zeros[SHA256_DIGEST_SIZE];
    if (!salt || !salt_length) {
        for (u32 index = 0; index < SHA256_DIGEST_SIZE; index++)
            zeros[index] = 0;
        salt = zeros;
        salt_length = SHA256_DIGEST_SIZE;
    }
    hmac_sha256_digest(salt, salt_length, key, key_length, prk);
}

int hkdf_sha256_expand(const u8 prk[SHA256_DIGEST_SIZE], const void *info,
                       u32 info_length, u8 *output, u32 length) {
    if (!prk || (!output && length) || (!info && info_length)) return -1;
    if (length > HKDF_SHA256_MAX_OUTPUT) return -1;
    u8 previous[SHA256_DIGEST_SIZE];
    u32 produced = 0;
    u8 counter = 1;
    while (produced < length) {
        struct hmac_sha256 context;
        hmac_sha256_init(&context, prk, SHA256_DIGEST_SIZE);
        // Every block except the first is chained through the previous one.
        if (produced) hmac_sha256_update(&context, previous, SHA256_DIGEST_SIZE);
        hmac_sha256_update(&context, info, info_length);
        hmac_sha256_update(&context, &counter, 1);
        hmac_sha256_final(&context, previous);
        crypto_zero(&context, sizeof(context));
        u32 remaining = length - produced;
        u32 take = remaining < SHA256_DIGEST_SIZE ? remaining : SHA256_DIGEST_SIZE;
        for (u32 index = 0; index < take; index++)
            output[produced + index] = previous[index];
        produced += take;
        counter++;
    }
    crypto_zero(previous, sizeof(previous));
    return 0;
}

static const u64 sha512_round_constants[80] = {
    0x428A2F98D728AE22ull, 0x7137449123EF65CDull, 0xB5C0FBCFEC4D3B2Full,
    0xE9B5DBA58189DBBCull, 0x3956C25BF348B538ull, 0x59F111F1B605D019ull,
    0x923F82A4AF194F9Bull, 0xAB1C5ED5DA6D8118ull, 0xD807AA98A3030242ull,
    0x12835B0145706FBEull, 0x243185BE4EE4B28Cull, 0x550C7DC3D5FFB4E2ull,
    0x72BE5D74F27B896Full, 0x80DEB1FE3B1696B1ull, 0x9BDC06A725C71235ull,
    0xC19BF174CF692694ull, 0xE49B69C19EF14AD2ull, 0xEFBE4786384F25E3ull,
    0x0FC19DC68B8CD5B5ull, 0x240CA1CC77AC9C65ull, 0x2DE92C6F592B0275ull,
    0x4A7484AA6EA6E483ull, 0x5CB0A9DCBD41FBD4ull, 0x76F988DA831153B5ull,
    0x983E5152EE66DFABull, 0xA831C66D2DB43210ull, 0xB00327C898FB213Full,
    0xBF597FC7BEEF0EE4ull, 0xC6E00BF33DA88FC2ull, 0xD5A79147930AA725ull,
    0x06CA6351E003826Full, 0x142929670A0E6E70ull, 0x27B70A8546D22FFCull,
    0x2E1B21385C26C926ull, 0x4D2C6DFC5AC42AEDull, 0x53380D139D95B3DFull,
    0x650A73548BAF63DEull, 0x766A0ABB3C77B2A8ull, 0x81C2C92E47EDAEE6ull,
    0x92722C851482353Bull, 0xA2BFE8A14CF10364ull, 0xA81A664BBC423001ull,
    0xC24B8B70D0F89791ull, 0xC76C51A30654BE30ull, 0xD192E819D6EF5218ull,
    0xD69906245565A910ull, 0xF40E35855771202Aull, 0x106AA07032BBD1B8ull,
    0x19A4C116B8D2D0C8ull, 0x1E376C085141AB53ull, 0x2748774CDF8EEB99ull,
    0x34B0BCB5E19B48A8ull, 0x391C0CB3C5C95A63ull, 0x4ED8AA4AE3418ACBull,
    0x5B9CCA4F7763E373ull, 0x682E6FF3D6B2B8A3ull, 0x748F82EE5DEFB2FCull,
    0x78A5636F43172F60ull, 0x84C87814A1F0AB72ull, 0x8CC702081A6439ECull,
    0x90BEFFFA23631E28ull, 0xA4506CEBDE82BDE9ull, 0xBEF9A3F7B2C67915ull,
    0xC67178F2E372532Bull, 0xCA273ECEEA26619Cull, 0xD186B8C721C0C207ull,
    0xEADA7DD6CDE0EB1Eull, 0xF57D4F7FEE6ED178ull, 0x06F067AA72176FBAull,
    0x0A637DC5A2C898A6ull, 0x113F9804BEF90DAEull, 0x1B710B35131C471Bull,
    0x28DB77F523047D84ull, 0x32CAAB7B40C72493ull, 0x3C9EBE0A15C9BEBCull,
    0x431D67C49C100D4Cull, 0x4CC5D4BECB3E42B6ull, 0x597F299CFC657E2Aull,
    0x5FCB6FAB3AD6FAECull, 0x6C44198C4A475817ull,
};

static u64 rotate_right64(u64 value, u32 bits) {
    return (value >> bits) | (value << (64u - bits));
}

static u64 load_be64(const u8 *source) {
    u64 value = 0;
    for (u32 index = 0; index < 8; index++)
        value = (value << 8) | source[index];
    return value;
}

static void sha512_compress(u64 state[8], const u8 block[SHA512_BLOCK_SIZE]) {
    u64 schedule[80];
    for (u32 index = 0; index < 16; index++)
        schedule[index] = load_be64(block + index * 8);
    for (u32 index = 16; index < 80; index++) {
        u64 previous = schedule[index - 15];
        u64 recent = schedule[index - 2];
        u64 s0 = rotate_right64(previous, 1) ^ rotate_right64(previous, 8) ^
            (previous >> 7);
        u64 s1 = rotate_right64(recent, 19) ^ rotate_right64(recent, 61) ^
            (recent >> 6);
        schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }
    u64 a = state[0];
    u64 b = state[1];
    u64 c = state[2];
    u64 d = state[3];
    u64 e = state[4];
    u64 f = state[5];
    u64 g = state[6];
    u64 h = state[7];
    for (u32 index = 0; index < 80; index++) {
        u64 s1 = rotate_right64(e, 14) ^ rotate_right64(e, 18) ^
            rotate_right64(e, 41);
        u64 choice = (e & f) ^ ((~e) & g);
        u64 temp1 = h + s1 + choice + sha512_round_constants[index] +
            schedule[index];
        u64 s0 = rotate_right64(a, 28) ^ rotate_right64(a, 34) ^
            rotate_right64(a, 39);
        u64 majority = (a & b) ^ (a & c) ^ (b & c);
        u64 temp2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
    crypto_zero(schedule, sizeof(schedule));
}

void sha384_init(struct sha384 *context) {
    if (!context) return;
    context->state[0] = 0xCBBB9D5DC1059ED8ull;
    context->state[1] = 0x629A292A367CD507ull;
    context->state[2] = 0x9159015A3070DD17ull;
    context->state[3] = 0x152FECD8F70E5939ull;
    context->state[4] = 0x67332667FFC00B31ull;
    context->state[5] = 0x8EB44A8768581511ull;
    context->state[6] = 0xDB0C2E0D64F98FA7ull;
    context->state[7] = 0x47B5481DBEFA4FA4ull;
    context->length = 0;
    context->buffered = 0;
    for (u32 index = 0; index < SHA512_BLOCK_SIZE; index++)
        context->block[index] = 0;
}

void sha384_update(struct sha384 *context, const void *data, u32 length) {
    if (!context || (!data && length)) return;
    const u8 *bytes = (const u8 *)data;
    context->length += (u64)length;
    while (length) {
        u32 space = SHA512_BLOCK_SIZE - context->buffered;
        u32 take = length < space ? length : space;
        for (u32 index = 0; index < take; index++)
            context->block[context->buffered + index] = bytes[index];
        context->buffered += take;
        bytes += take;
        length -= take;
        if (context->buffered == SHA512_BLOCK_SIZE) {
            sha512_compress(context->state, context->block);
            context->buffered = 0;
        }
    }
}

void sha384_final(struct sha384 *context, u8 digest[SHA384_DIGEST_SIZE]) {
    if (!context || !digest) return;
    u64 bits = context->length * 8ull;
    context->block[context->buffered++] = 0x80u;
    // The length field is 16 bytes here, not 8, so the block that cannot hold
    // it is flushed first.
    if (context->buffered > SHA512_BLOCK_SIZE - 16u) {
        while (context->buffered < SHA512_BLOCK_SIZE)
            context->block[context->buffered++] = 0;
        sha512_compress(context->state, context->block);
        context->buffered = 0;
    }
    while (context->buffered < SHA512_BLOCK_SIZE - 8u)
        context->block[context->buffered++] = 0;
    for (u32 index = 0; index < 8; index++)
        context->block[SHA512_BLOCK_SIZE - 1u - index] =
            (u8)(bits >> (index * 8));
    sha512_compress(context->state, context->block);
    // SHA-384 keeps only the first six words of the state.
    for (u32 index = 0; index < 6; index++)
        for (u32 byte = 0; byte < 8; byte++)
            digest[index * 8 + byte] =
                (u8)(context->state[index] >> (56u - byte * 8u));
    crypto_zero(context->block, SHA512_BLOCK_SIZE);
}

void sha384_digest(const void *data, u32 length,
                   u8 digest[SHA384_DIGEST_SIZE]) {
    struct sha384 context;
    sha384_init(&context);
    sha384_update(&context, data, length);
    sha384_final(&context, digest);
    crypto_zero(&context, sizeof(context));
}
