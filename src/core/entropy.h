#ifndef ENTROPY_H
#define ENTROPY_H

#include "types.h"

#define ENTROPY_FILL_MAX 4096u

/* Seeds and generates the kernel DRBG. Must run once before entropy_fill;
   safe to call on every CPU but only the first call installs the state. */
void entropy_init(void);

/* Fills buffer with length pseudorandom bytes (length bounded by
   ENTROPY_FILL_MAX). 0 on success, -1 on rejection. */
int entropy_fill(void *buffer, u32 length);

/* Raw RFC 8439 ChaCha20 block function, exported for the known-answer
   test in the test image. Pure: no DRBG state involved. */
void entropy_chacha20_block(const u8 key[32], u32 counter,
                            const u8 nonce[12], u8 out[64]);

#endif
