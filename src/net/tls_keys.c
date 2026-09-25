#include "tls_keys.h"
#include "crypto.h"

#define LABEL_PREFIX "tls13 "
#define LABEL_PREFIX_LENGTH 6u

// struct { uint16 length; opaque label<7..255>; opaque context<0..255>; }
// with both variable fields carrying a one byte length in front.
#define HKDF_LABEL_MAX (2u + 1u + LABEL_PREFIX_LENGTH + TLS_MAX_LABEL + 1u + \
                        TLS_SECRET_SIZE)

static u32 label_length(const char *label) {
    u32 length = 0;
    while (label[length] && length <= TLS_MAX_LABEL) length++;
    return length;
}

int tls_hkdf_expand_label(const u8 secret[TLS_SECRET_SIZE], const char *label,
                          const u8 *context, u32 context_length, u8 *out,
                          u32 length) {
    if (!secret || !label || (!out && length)) return -1;
    if (!context && context_length) return -1;
    if (context_length > TLS_SECRET_SIZE) return -1;
    u32 name_length = label_length(label);
    if (!name_length || name_length > TLS_MAX_LABEL) return -1;
    if (length > 0xFFFFu) return -1;

    u8 info[HKDF_LABEL_MAX];
    u32 cursor = 0;
    info[cursor++] = (u8)(length >> 8);
    info[cursor++] = (u8)length;
    info[cursor++] = (u8)(LABEL_PREFIX_LENGTH + name_length);
    for (u32 index = 0; index < LABEL_PREFIX_LENGTH; index++)
        info[cursor++] = (u8)LABEL_PREFIX[index];
    for (u32 index = 0; index < name_length; index++)
        info[cursor++] = (u8)label[index];
    info[cursor++] = (u8)context_length;
    for (u32 index = 0; index < context_length; index++)
        info[cursor++] = context[index];

    int result = hkdf_sha256_expand(secret, info, cursor, out, length);
    crypto_zero(info, sizeof(info));
    return result;
}

int tls_derive_secret(const u8 secret[TLS_SECRET_SIZE], const char *label,
                      const u8 transcript[TLS_SECRET_SIZE],
                      u8 out[TLS_SECRET_SIZE]) {
    if (!transcript) return -1;
    return tls_hkdf_expand_label(secret, label, transcript, TLS_SECRET_SIZE,
                                 out, TLS_SECRET_SIZE);
}

void tls_key_schedule_init(struct tls_key_schedule *schedule) {
    if (!schedule) return;
    u8 *bytes = (u8 *)schedule;
    for (u32 index = 0; index < sizeof(*schedule); index++) bytes[index] = 0;
    u8 zeros[TLS_SECRET_SIZE];
    for (u32 index = 0; index < TLS_SECRET_SIZE; index++) zeros[index] = 0;
    // Without a pre-shared key both the salt and the input are zeros, which is
    // what RFC 8446 specifies rather than a shortcut.
    hkdf_sha256_extract(0, 0, zeros, TLS_SECRET_SIZE, schedule->early_secret);
}

int tls_key_schedule_handshake(struct tls_key_schedule *schedule,
                               const u8 *shared_secret, u32 shared_length,
                               const u8 transcript[TLS_SECRET_SIZE]) {
    if (!schedule || !shared_secret || !shared_length || !transcript)
        return -1;
    u8 empty_hash[TLS_SECRET_SIZE];
    sha256_digest(0, 0, empty_hash);
    u8 derived[TLS_SECRET_SIZE];
    // Each stage is salted with the previous secret passed through
    // Derive-Secret over an empty transcript, not with the secret itself.
    if (tls_derive_secret(schedule->early_secret, "derived", empty_hash,
                          derived))
        return -1;
    hkdf_sha256_extract(derived, TLS_SECRET_SIZE, shared_secret, shared_length,
                        schedule->handshake_secret);
    crypto_zero(derived, sizeof(derived));

    if (tls_derive_secret(schedule->handshake_secret, "c hs traffic",
                          transcript, schedule->client_handshake_traffic))
        return -1;
    return tls_derive_secret(schedule->handshake_secret, "s hs traffic",
                             transcript, schedule->server_handshake_traffic);
}

int tls_key_schedule_application(struct tls_key_schedule *schedule,
                                 const u8 transcript[TLS_SECRET_SIZE]) {
    if (!schedule || !transcript) return -1;
    u8 empty_hash[TLS_SECRET_SIZE];
    sha256_digest(0, 0, empty_hash);
    u8 derived[TLS_SECRET_SIZE];
    if (tls_derive_secret(schedule->handshake_secret, "derived", empty_hash,
                          derived))
        return -1;
    u8 zeros[TLS_SECRET_SIZE];
    for (u32 index = 0; index < TLS_SECRET_SIZE; index++) zeros[index] = 0;
    hkdf_sha256_extract(derived, TLS_SECRET_SIZE, zeros, TLS_SECRET_SIZE,
                        schedule->master_secret);
    crypto_zero(derived, sizeof(derived));

    if (tls_derive_secret(schedule->master_secret, "c ap traffic", transcript,
                          schedule->client_application_traffic))
        return -1;
    return tls_derive_secret(schedule->master_secret, "s ap traffic",
                             transcript,
                             schedule->server_application_traffic);
}

int tls_traffic_keys(const u8 secret[TLS_SECRET_SIZE], u8 key[TLS_KEY_SIZE],
                     u8 iv[TLS_IV_SIZE]) {
    if (!secret || !key || !iv) return -1;
    if (tls_hkdf_expand_label(secret, "key", 0, 0, key, TLS_KEY_SIZE))
        return -1;
    return tls_hkdf_expand_label(secret, "iv", 0, 0, iv, TLS_IV_SIZE);
}

void tls_key_schedule_clear(struct tls_key_schedule *schedule) {
    if (!schedule) return;
    crypto_zero(schedule, sizeof(*schedule));
}
