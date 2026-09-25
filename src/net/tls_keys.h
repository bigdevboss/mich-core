#ifndef MICH_NET_TLS_KEYS_H
#define MICH_NET_TLS_KEYS_H

#include "types.h"
#include "sha256.h"

#define TLS_SECRET_SIZE SHA256_DIGEST_SIZE
#define TLS_KEY_SIZE 16u
#define TLS_IV_SIZE 12u

// The label a certificate or a peer never sees: HkdfLabel carries a one byte
// length in front of it, so nothing longer than this can be encoded.
#define TLS_MAX_LABEL 32u

// TLS 1.3 grows a tree of secrets rather than deriving everything from one
// value (RFC 8446 section 7.1). Handshake keys and application keys come from
// different levels, so losing one set does not hand over the other.
struct tls_key_schedule {
    u8 early_secret[TLS_SECRET_SIZE];
    u8 handshake_secret[TLS_SECRET_SIZE];
    u8 master_secret[TLS_SECRET_SIZE];
    u8 client_handshake_traffic[TLS_SECRET_SIZE];
    u8 server_handshake_traffic[TLS_SECRET_SIZE];
    u8 client_application_traffic[TLS_SECRET_SIZE];
    u8 server_application_traffic[TLS_SECRET_SIZE];
};

// HKDF-Expand with the info field TLS 1.3 prescribes: a length, the label
// prefixed with "tls13 ", and a context. This is protocol formatting, which is
// why it lives here and not next to the HKDF primitive.
// 0 on success, -1 when a bound is exceeded.
int tls_hkdf_expand_label(const u8 secret[TLS_SECRET_SIZE], const char *label,
                          const u8 *context, u32 context_length, u8 *out,
                          u32 length);

// Derive-Secret: expand-label over a transcript hash rather than a literal.
int tls_derive_secret(const u8 secret[TLS_SECRET_SIZE], const char *label,
                      const u8 transcript[TLS_SECRET_SIZE],
                      u8 out[TLS_SECRET_SIZE]);

// No pre-shared key, so the early secret extracts from zeros. Must run before
// the handshake stage.
void tls_key_schedule_init(struct tls_key_schedule *schedule);

// Mixes in the (EC)DHE shared secret and derives both handshake traffic
// secrets from the transcript through ServerHello.
int tls_key_schedule_handshake(struct tls_key_schedule *schedule,
                               const u8 *shared_secret, u32 shared_length,
                               const u8 transcript[TLS_SECRET_SIZE]);

// Derives the application traffic secrets from the transcript through the
// server Finished.
int tls_key_schedule_application(struct tls_key_schedule *schedule,
                                 const u8 transcript[TLS_SECRET_SIZE]);

// Turns a traffic secret into the record protection key and static IV.
int tls_traffic_keys(const u8 secret[TLS_SECRET_SIZE], u8 key[TLS_KEY_SIZE],
                     u8 iv[TLS_IV_SIZE]);

void tls_key_schedule_clear(struct tls_key_schedule *schedule);

#endif
