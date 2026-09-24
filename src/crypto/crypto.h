#ifndef MICH_CRYPTO_H
#define MICH_CRYPTO_H

#include "types.h"

// Compares two buffers in time that depends only on length. A plain loop that
// stops at the first difference leaks the position of that difference, which
// is enough to recover an authentication tag byte by byte. Returns 1 when the
// buffers match.
int crypto_equal(const void *left, const void *right, u32 length);

// Overwrites a buffer that held key material. A plain loop over a buffer that
// is never read again is dead code, and the compiler is free to delete it.
void crypto_zero(void *buffer, u32 length);

#endif
