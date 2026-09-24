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
