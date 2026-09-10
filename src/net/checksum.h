#ifndef NET_CHECKSUM_H
#define NET_CHECKSUM_H

#include "types.h"

void *memcpy(void *dst, const void *src, usize_t length);

#if defined(__SSE2__)
// Skip the malloc-based _mm_malloc declarations: freestanding builds have
// no stdlib.h under -nostdinc.
#ifndef _MM_MALLOC_H_INCLUDED
#define _MM_MALLOC_H_INCLUDED
#endif
#include <emmintrin.h>
#endif

static inline void net_copy(void *dst, const void *src, u32 length) {
    if (length)
        memcpy(dst, src, length);
}

static inline u16 net_checksum_finish(u32 sum) {
    sum = (sum & 0xFFFFu) + (sum >> 16);
    sum += sum >> 16;
    return (u16)~sum;
}

#if defined(__SSE2__)
static inline u32 net_checksum_sad_value(__m128i all_acc, __m128i even_acc) {
    u32 total[4], even[4];
    _mm_storeu_si128((__m128i *)total, all_acc);
    _mm_storeu_si128((__m128i *)even, even_acc);
    u32 all = total[0] + total[2];
    u32 ev = even[0] + even[2];
    return ev * 256u + (all - ev);
}

static inline void net_checksum_sad_add(__m128i v, __m128i even_mask,
                                        __m128i *all_acc, __m128i *even_acc) {
    __m128i zero = _mm_setzero_si128();
    *all_acc = _mm_add_epi64(*all_acc, _mm_sad_epu8(v, zero));
    *even_acc = _mm_add_epi64(*even_acc,
        _mm_sad_epu8(_mm_and_si128(v, even_mask), zero));
}
#endif

static inline u32 net_checksum_sum_tail(u32 sum, const u8 *bytes, u32 length) {
    while (length >= 8) {
        sum += ((u32)bytes[0] << 8) | bytes[1];
        sum += ((u32)bytes[2] << 8) | bytes[3];
        sum += ((u32)bytes[4] << 8) | bytes[5];
        sum += ((u32)bytes[6] << 8) | bytes[7];
        bytes += 8;
        length -= 8;
    }
    while (length >= 2) {
        sum += ((u32)bytes[0] << 8) | bytes[1];
        bytes += 2;
        length -= 2;
    }
    if (length) sum += (u32)bytes[0] << 8;
    return sum;
}

// 16-bit one's-complement sum over big-endian byte pairs, without the
// final fold and complement. Callers keep total input within the
// 65535-byte protocol limit, so a 32-bit accumulator cannot overflow.
// A big-endian pair (b0<<8|b1) is b0*256 + b1, so the block sum is
// 256*sum(even bytes) + sum(odd bytes). SAD accumulators stay in XMM
// across the loop; the scalar tail covers the leftover bytes.
static inline u32 net_checksum_sum(u32 sum, const u8 *bytes, u32 length) {
#if defined(__SSE2__)
    if (length >= 16) {
        __m128i even_mask = _mm_setr_epi8(0xff, 0, 0xff, 0, 0xff, 0, 0xff, 0,
                                          0xff, 0, 0xff, 0, 0xff, 0, 0xff, 0);
        __m128i all_acc = _mm_setzero_si128();
        __m128i even_acc = _mm_setzero_si128();
        while (length >= 16) {
            __m128i v = _mm_loadu_si128((const __m128i *)bytes);
            net_checksum_sad_add(v, even_mask, &all_acc, &even_acc);
            bytes += 16;
            length -= 16;
        }
        sum += net_checksum_sad_value(all_acc, even_acc);
    }
#endif
    return net_checksum_sum_tail(sum, bytes, length);
}

static inline u32 net_checksum_copy(u32 sum, u8 *dst, const u8 *src,
                                    u32 length) {
#if defined(__SSE2__)
    if (length >= 16) {
        __m128i even_mask = _mm_setr_epi8(0xff, 0, 0xff, 0, 0xff, 0, 0xff, 0,
                                          0xff, 0, 0xff, 0, 0xff, 0, 0xff, 0);
        __m128i all_acc = _mm_setzero_si128();
        __m128i even_acc = _mm_setzero_si128();
        while (length >= 16) {
            __m128i v = _mm_loadu_si128((const __m128i *)src);
            _mm_storeu_si128((__m128i *)dst, v);
            net_checksum_sad_add(v, even_mask, &all_acc, &even_acc);
            src += 16;
            dst += 16;
            length -= 16;
        }
        sum += net_checksum_sad_value(all_acc, even_acc);
    }
#endif
    while (length >= 8) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
        dst[4] = src[4];
        dst[5] = src[5];
        dst[6] = src[6];
        dst[7] = src[7];
        sum += ((u32)src[0] << 8) | src[1];
        sum += ((u32)src[2] << 8) | src[3];
        sum += ((u32)src[4] << 8) | src[5];
        sum += ((u32)src[6] << 8) | src[7];
        src += 8;
        dst += 8;
        length -= 8;
    }
    while (length >= 2) {
        dst[0] = src[0];
        dst[1] = src[1];
        sum += ((u32)src[0] << 8) | src[1];
        src += 2;
        dst += 2;
        length -= 2;
    }
    if (length) {
        dst[0] = src[0];
        sum += (u32)src[0] << 8;
    }
    return sum;
}

#endif
