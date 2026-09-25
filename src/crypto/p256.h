#ifndef MICH_CRYPTO_P256_H
#define MICH_CRYPTO_P256_H

#include "types.h"

#define P256_SCALAR_SIZE 32u
#define P256_COORD_SIZE 32u
// SEC 1 uncompressed encoding: 0x04 followed by both coordinates.
#define P256_PUBLIC_KEY_SIZE 65u

// Verifies an ECDSA signature over P-256 (FIPS 186-4). Only verification is
// implemented: every input here is public, so unlike x25519 this code carries
// no secret and does not need to avoid branches.
//
// r and s arrive as plain 32 byte big-endian integers. Unwrapping the DER
// SEQUENCE they travel in belongs to the X.509 layer, not here.
//
// Returns 0 when the signature is valid, -1 otherwise, including a public key
// that is not a point on the curve.
int p256_verify(const u8 public_key[P256_PUBLIC_KEY_SIZE],
                const u8 digest[P256_SCALAR_SIZE],
                const u8 r[P256_SCALAR_SIZE], const u8 s[P256_SCALAR_SIZE]);

#endif
