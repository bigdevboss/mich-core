#ifndef MICH_CRYPTO_RSA_H
#define MICH_CRYPTO_RSA_H

#include "types.h"

// 4096 bits covers every root in the trust store and the largest key sizes
// seen in the wild. Anything longer is refused rather than silently truncated.
#define RSA_MAX_MODULUS_BYTES 512u
#define RSA_MAX_LIMBS (RSA_MAX_MODULUS_BYTES / 8u)

struct rsa_public_key {
    u64 modulus[RSA_MAX_LIMBS];
    u64 montgomery_r2[RSA_MAX_LIMBS];
    u32 limbs;
    u32 modulus_bytes;
    u32 exponent;
    u64 n0;
};

// Only verification is implemented, so every value here is public and the code
// does not need to hide timing. modulus is big-endian, exponent is the small
// integer RSA keys actually use (65537 in practice).
// 0 on success, -1 when the modulus is malformed or too large.
int rsa_public_key_init(struct rsa_public_key *key, const u8 *modulus,
                        u32 modulus_bytes, u32 exponent);

// RSASSA-PKCS1-v1_5 with SHA-256, which is what certificate chains are signed
// with. 0 when the signature is valid.
int rsa_pkcs1_verify_sha256(const struct rsa_public_key *key,
                            const u8 digest[32], const u8 *signature,
                            u32 signature_bytes);

// RSASSA-PSS with SHA-256, MGF1-SHA256 and a 32 byte salt: the form TLS 1.3
// requires for CertificateVerify.
int rsa_pss_verify_sha256(const struct rsa_public_key *key,
                          const u8 digest[32], const u8 *signature,
                          u32 signature_bytes);

#endif
