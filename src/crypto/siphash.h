#ifndef MICH_CRYPTO_SIPHASH_H
#define MICH_CRYPTO_SIPHASH_H

#include "types.h"

#define SIPHASH_KEY_SIZE 16u

// SipHash-2-4 keyed hash (Aumasson and Bernstein). Fast and keyed, and without
// the key an off-path party cannot predict an output from other outputs. The
// TCP ISN generator relies on exactly that property under RFC 6528.
u64 siphash_2_4(const u8 key[SIPHASH_KEY_SIZE], const void *data, u32 length);

#endif
