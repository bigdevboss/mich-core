#ifndef MICH_NET_TLS_RECORD_H
#define MICH_NET_TLS_RECORD_H

#include "types.h"
#include "tls_keys.h"
#include "gcm.h"

#define TLS_RECORD_HEADER_SIZE 5u
#define TLS_RECORD_MAX_PLAINTEXT 16384u
// The ciphertext may exceed the plaintext limit by the inner type byte, any
// padding and the tag. RFC 8446 caps the excess at 256.
#define TLS_RECORD_MAX_CIPHERTEXT (TLS_RECORD_MAX_PLAINTEXT + 256u)

#define TLS_CONTENT_CHANGE_CIPHER_SPEC 20u
#define TLS_CONTENT_ALERT 21u
#define TLS_CONTENT_HANDSHAKE 22u
#define TLS_CONTENT_APPLICATION_DATA 23u

// One direction of one connection. The sequence number is part of the nonce,
// so a context must never be reused for a second key.
struct tls_record_context {
    struct aes128_gcm cipher;
    u8 iv[TLS_IV_SIZE];
    u64 sequence;
    int exhausted;
};

int tls_record_init(struct tls_record_context *context,
                    const u8 secret[TLS_SECRET_SIZE]);

// Encrypts one record. content_type goes inside the ciphertext; the header
// always claims application_data, which is what hides handshake boundaries
// from an observer.
//
// out receives the full record, header included, and needs room for
// TLS_RECORD_HEADER_SIZE + length + 1 + GCM_TAG_SIZE bytes.
// 0 on success, -1 on a bad argument or an exhausted sequence number.
int tls_record_seal(struct tls_record_context *context, u8 content_type,
                    const u8 *plaintext, u32 length, u8 *out,
                    u32 *out_length);

// Decrypts one record, header included. Reports the content type that was
// hidden inside. The plaintext buffer needs room for length bytes.
int tls_record_open(struct tls_record_context *context, const u8 *record,
                    u32 record_length, u8 *plaintext, u32 *plaintext_length,
                    u8 *content_type);

void tls_record_clear(struct tls_record_context *context);

#endif
