#ifndef MICH_CRYPTO_SHA256_H
#define MICH_CRYPTO_SHA256_H

#include "types.h"

#define SHA256_DIGEST_SIZE 32u
#define SHA256_BLOCK_SIZE 64u

// HKDF-Expand counts output blocks in a single byte, so it cannot produce more
// than 255 hashes worth of key material (RFC 5869 section 2.3).
#define HKDF_SHA256_MAX_OUTPUT (255u * SHA256_DIGEST_SIZE)

// Plain data, no pointers into itself, so a caller can take a snapshot of a
// running hash by copying the struct. TLS 1.3 needs exactly that: the
// transcript hash is read at several points while the transcript keeps
// growing, and sha256_final would otherwise destroy the state.
struct sha256 {
    u32 state[8];
    u64 length;
    u32 buffered;
    u8 block[SHA256_BLOCK_SIZE];
};

void sha256_init(struct sha256 *context);
void sha256_update(struct sha256 *context, const void *data, u32 length);
void sha256_final(struct sha256 *context, u8 digest[SHA256_DIGEST_SIZE]);
void sha256_digest(const void *data, u32 length,
                   u8 digest[SHA256_DIGEST_SIZE]);

struct hmac_sha256 {
    struct sha256 inner;
    struct sha256 outer;
};

void hmac_sha256_init(struct hmac_sha256 *context, const void *key,
                      u32 key_length);
void hmac_sha256_update(struct hmac_sha256 *context, const void *data,
                        u32 length);
void hmac_sha256_final(struct hmac_sha256 *context,
                       u8 mac[SHA256_DIGEST_SIZE]);
void hmac_sha256_digest(const void *key, u32 key_length, const void *data,
                        u32 length, u8 mac[SHA256_DIGEST_SIZE]);

// salt may be empty, which RFC 5869 defines as a block of zeros.
void hkdf_sha256_extract(const void *salt, u32 salt_length, const void *key,
                         u32 key_length, u8 prk[SHA256_DIGEST_SIZE]);

// 0 on success, -1 when length exceeds HKDF_SHA256_MAX_OUTPUT.
int hkdf_sha256_expand(const u8 prk[SHA256_DIGEST_SIZE], const void *info,
                       u32 info_length, u8 *output, u32 length);

#endif
