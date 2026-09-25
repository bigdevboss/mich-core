#include "aes.h"
#include "crypto.h"

// wmmintrin pulls in mm_malloc.h, which wants a hosted stdlib. Nothing here
// allocates, so the guard is set to keep that header out.
#define _MM_MALLOC_H_INCLUDED
#include <wmmintrin.h>
#include <emmintrin.h>

#define CPUID_ECX_PCLMULQDQ (1u << 1)
#define CPUID_ECX_SSSE3 (1u << 9)
#define CPUID_ECX_AES (1u << 25)

int aes_hardware_available(void) {
    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1u), "c"(0u));
    // SSSE3 belongs in this check too: GHASH byte-swaps its blocks with
    // pshufb, so a CPU with AES-NI but no SSSE3 would still take a #UD.
    return (ecx & CPUID_ECX_AES) && (ecx & CPUID_ECX_PCLMULQDQ) &&
        (ecx & CPUID_ECX_SSSE3);
}

// aeskeygenassist returns the transformed word in lane 3, and each round key
// is built from the previous one by xoring in the running prefix. The shifts
// below are that prefix, not a byte swap.
static __m128i expand_step(__m128i previous, __m128i assist) {
    assist = _mm_shuffle_epi32(assist, 0xFF);
    previous = _mm_xor_si128(previous, _mm_slli_si128(previous, 4));
    previous = _mm_xor_si128(previous, _mm_slli_si128(previous, 4));
    previous = _mm_xor_si128(previous, _mm_slli_si128(previous, 4));
    return _mm_xor_si128(previous, assist);
}

void aes128_init(struct aes128 *context, const u8 key[AES128_KEY_SIZE]) {
    if (!context || !key) return;
    __m128i round = _mm_loadu_si128((const __m128i *)key);
    __m128i *keys = (__m128i *)context->round_keys;
    keys[0] = round;
    round = expand_step(round, _mm_aeskeygenassist_si128(round, 0x01));
    keys[1] = round;
    round = expand_step(round, _mm_aeskeygenassist_si128(round, 0x02));
    keys[2] = round;
    round = expand_step(round, _mm_aeskeygenassist_si128(round, 0x04));
    keys[3] = round;
    round = expand_step(round, _mm_aeskeygenassist_si128(round, 0x08));
    keys[4] = round;
    round = expand_step(round, _mm_aeskeygenassist_si128(round, 0x10));
    keys[5] = round;
    round = expand_step(round, _mm_aeskeygenassist_si128(round, 0x20));
    keys[6] = round;
    round = expand_step(round, _mm_aeskeygenassist_si128(round, 0x40));
    keys[7] = round;
    round = expand_step(round, _mm_aeskeygenassist_si128(round, 0x80));
    keys[8] = round;
    round = expand_step(round, _mm_aeskeygenassist_si128(round, 0x1B));
    keys[9] = round;
    round = expand_step(round, _mm_aeskeygenassist_si128(round, 0x36));
    keys[10] = round;
}

void aes128_encrypt_block(const struct aes128 *context,
                          const u8 input[AES_BLOCK_SIZE],
                          u8 output[AES_BLOCK_SIZE]) {
    if (!context || !input || !output) return;
    const __m128i *keys = (const __m128i *)context->round_keys;
    __m128i block = _mm_loadu_si128((const __m128i *)input);
    block = _mm_xor_si128(block, keys[0]);
    for (u32 round = 1; round < AES128_ROUNDS; round++)
        block = _mm_aesenc_si128(block, keys[round]);
    block = _mm_aesenclast_si128(block, keys[AES128_ROUNDS]);
    _mm_storeu_si128((__m128i *)output, block);
}
