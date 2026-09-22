#include "types.h"

#if defined(__SSE2__)
#ifndef _MM_MALLOC_H_INCLUDED
#define _MM_MALLOC_H_INCLUDED
#endif
#include <emmintrin.h>
#endif

// Freestanding kernel: no libc. The compiler lowers some struct copies to a
// memcpy call (the lowering depends on the host gcc version), so the symbol
// must exist for the link. The x86-64 build uses SSE2 16-byte moves.
void *memcpy(void *dst, const void *src, usize_t length) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
#if defined(__SSE2__)
    while (length >= 16) {
        _mm_storeu_si128((__m128i *)d, _mm_loadu_si128((const __m128i *)s));
        d += 16;
        s += 16;
        length -= 16;
    }
#else
    typedef struct { u8 b[16]; } copy16;
    while (length >= 16) {
        *(copy16 *)d = *(const copy16 *)s;
        d += 16;
        s += 16;
        length -= 16;
    }
#endif
    while (length >= 4) {
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = s[3];
        d += 4;
        s += 4;
        length -= 4;
    }
    while (length--)
        *d++ = *s++;
    return dst;
}
