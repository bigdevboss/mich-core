#include "tls_record.h"
#include "crypto.h"

#define LEGACY_VERSION_HIGH 0x03u
#define LEGACY_VERSION_LOW 0x03u

int tls_record_init(struct tls_record_context *context,
                    const u8 secret[TLS_SECRET_SIZE]) {
    if (!context || !secret) return -1;
    u8 key[TLS_KEY_SIZE];
    if (tls_traffic_keys(secret, key, context->iv)) return -1;
    aes128_gcm_init(&context->cipher, key);
    crypto_zero(key, sizeof(key));
    context->sequence = 0;
    context->exhausted = 0;
    return 0;
}

// The per-record nonce is the static IV with the sequence number xored into
// its low eight bytes (RFC 8446 section 5.3).
static void build_nonce(const struct tls_record_context *context,
                        u8 nonce[TLS_IV_SIZE]) {
    for (u32 index = 0; index < TLS_IV_SIZE; index++)
        nonce[index] = context->iv[index];
    for (u32 index = 0; index < 8; index++)
        nonce[TLS_IV_SIZE - 1u - index] ^=
            (u8)(context->sequence >> (index * 8));
}

static void build_header(u8 header[TLS_RECORD_HEADER_SIZE], u32 length) {
    header[0] = TLS_CONTENT_APPLICATION_DATA;
    header[1] = LEGACY_VERSION_HIGH;
    header[2] = LEGACY_VERSION_LOW;
    header[3] = (u8)(length >> 8);
    header[4] = (u8)length;
}

// A repeated nonce under one key lets an attacker recover the GHASH key and
// forge tags for anything. The counter therefore stops the connection instead
// of wrapping.
static int consume_sequence(struct tls_record_context *context) {
    if (context->exhausted) return -1;
    if (context->sequence == 0xFFFFFFFFFFFFFFFFull) {
        context->exhausted = 1;
        return -1;
    }
    context->sequence++;
    return 0;
}

int tls_record_seal(struct tls_record_context *context, u8 content_type,
                    const u8 *plaintext, u32 length, u8 *out,
                    u32 *out_length) {
    if (!context || !out || !out_length) return -1;
    if (!plaintext && length) return -1;
    if (length > TLS_RECORD_MAX_PLAINTEXT) return -1;
    if (context->exhausted) return -1;

    // Inner plaintext is the content followed by its real type. No padding is
    // added here; the peer is required to handle any amount of it, and adding
    // none keeps the record minimal.
    u32 inner_length = length + 1u;
    u32 body_length = inner_length + GCM_TAG_SIZE;
    if (body_length > TLS_RECORD_MAX_CIPHERTEXT) return -1;

    static u8 inner[TLS_RECORD_MAX_CIPHERTEXT];
    for (u32 index = 0; index < length; index++) inner[index] = plaintext[index];
    inner[length] = content_type;

    u8 header[TLS_RECORD_HEADER_SIZE];
    build_header(header, body_length);
    u8 nonce[TLS_IV_SIZE];
    build_nonce(context, nonce);

    for (u32 index = 0; index < TLS_RECORD_HEADER_SIZE; index++)
        out[index] = header[index];
    if (aes128_gcm_seal(&context->cipher, nonce, header,
                        TLS_RECORD_HEADER_SIZE, inner, inner_length,
                        out + TLS_RECORD_HEADER_SIZE,
                        out + TLS_RECORD_HEADER_SIZE + inner_length)) {
        crypto_zero(inner, inner_length);
        return -1;
    }
    crypto_zero(inner, inner_length);
    crypto_zero(nonce, sizeof(nonce));
    if (consume_sequence(context)) return -1;
    *out_length = TLS_RECORD_HEADER_SIZE + body_length;
    return 0;
}

int tls_record_open(struct tls_record_context *context, const u8 *record,
                    u32 record_length, u8 *plaintext, u32 *plaintext_length,
                    u8 *content_type) {
    if (!context || !record || !plaintext_length || !content_type) return -1;
    if (context->exhausted) return -1;
    if (record_length < TLS_RECORD_HEADER_SIZE + 1u + GCM_TAG_SIZE) return -1;

    u32 body_length = ((u32)record[3] << 8) | record[4];
    if (body_length + TLS_RECORD_HEADER_SIZE != record_length) return -1;
    if (body_length > TLS_RECORD_MAX_CIPHERTEXT) return -1;
    if (record[0] != TLS_CONTENT_APPLICATION_DATA) return -1;

    u32 inner_length = body_length - GCM_TAG_SIZE;
    if (!plaintext && inner_length) return -1;

    u8 nonce[TLS_IV_SIZE];
    build_nonce(context, nonce);
    static u8 inner[TLS_RECORD_MAX_CIPHERTEXT];
    int opened = aes128_gcm_open(&context->cipher, nonce, record,
                                 TLS_RECORD_HEADER_SIZE,
                                 record + TLS_RECORD_HEADER_SIZE,
                                 inner_length,
                                 record + TLS_RECORD_HEADER_SIZE +
                                     inner_length,
                                 inner);
    crypto_zero(nonce, sizeof(nonce));
    if (opened) return -1;

    // The real type sits after the content and before any padding, so it is
    // found by scanning back over the zeros. A record that is all padding
    // carries no type and is a protocol error.
    u32 position = inner_length;
    while (position && !inner[position - 1u]) position--;
    if (!position) {
        crypto_zero(inner, inner_length);
        return -1;
    }
    *content_type = inner[position - 1u];
    *plaintext_length = position - 1u;
    for (u32 index = 0; index < *plaintext_length; index++)
        plaintext[index] = inner[index];
    crypto_zero(inner, inner_length);
    // Only a successfully opened record advances the counter, so a forged one
    // cannot push the connection out of step with the peer.
    return consume_sequence(context);
}

void tls_record_clear(struct tls_record_context *context) {
    if (!context) return;
    crypto_zero(context, sizeof(*context));
}
