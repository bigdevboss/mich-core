#ifndef MICH_CRYPTO_X25519_H
#define MICH_CRYPTO_X25519_H

#include "types.h"

#define X25519_KEY_SIZE 32u

// Scalar multiplication on Curve25519 (RFC 7748). The scalar is clamped and
// the u-coordinate has its top bit masked, so any 32 byte string is accepted
// as input rather than rejected as non-canonical.
//
// Returns -1 when the result is the all-zero value, which happens exactly when
// the peer supplied a point of small order. RFC 7748 leaves that check
// optional, but TLS 1.3 requires the handshake to abort, so it is done here
// and not left to the caller to remember.
int x25519(u8 out[X25519_KEY_SIZE], const u8 scalar[X25519_KEY_SIZE],
           const u8 point[X25519_KEY_SIZE]);

// Derives the public key for a private scalar: the same operation against the
// base point u=9.
int x25519_base(u8 out[X25519_KEY_SIZE], const u8 scalar[X25519_KEY_SIZE]);

#endif
