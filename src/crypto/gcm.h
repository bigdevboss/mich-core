#ifndef MICH_CRYPTO_GCM_H
#define MICH_CRYPTO_GCM_H

#include "types.h"
#include "aes.h"

#define GCM_NONCE_SIZE 12u
#define GCM_TAG_SIZE 16u

// TLS 1.3 never uses another nonce length, and a 96-bit nonce is the only one
// GCM takes without hashing it down first.
struct aes128_gcm {
    struct aes128 cipher;
    u8 hash_key[AES_BLOCK_SIZE];
};

void aes128_gcm_init(struct aes128_gcm *context, const u8 key[AES128_KEY_SIZE]);

// Encrypts in place is not supported: plaintext and ciphertext must not
// overlap. Writes GCM_TAG_SIZE bytes of tag. 0 on success.
int aes128_gcm_seal(const struct aes128_gcm *context,
                    const u8 nonce[GCM_NONCE_SIZE], const void *aad,
                    u32 aad_length, const void *plaintext, u32 length,
                    u8 *ciphertext, u8 tag[GCM_TAG_SIZE]);

// Verifies the tag before writing a single plaintext byte out, so a caller
// that ignores the return value still cannot read forged data. Returns 0 when
// the tag matched, -1 otherwise, and leaves the output untouched on failure.
int aes128_gcm_open(const struct aes128_gcm *context,
                    const u8 nonce[GCM_NONCE_SIZE], const void *aad,
                    u32 aad_length, const void *ciphertext, u32 length,
                    const u8 tag[GCM_TAG_SIZE], u8 *plaintext);

#endif
