#include "gcm.h"
#include "crypto.h"

// wmmintrin pulls in mm_malloc.h, which wants a hosted stdlib. Nothing here
// allocates, so the guard is set to keep that header out.
#define _MM_MALLOC_H_INCLUDED
#include <wmmintrin.h>
#include <emmintrin.h>
#include <tmmintrin.h>

// GCM numbers bits the other way round from the little-endian registers the
// instructions work in, so every block is byte-reversed on the way in and out
// of the multiplier.
static const u8 byte_swap_mask[16] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
};

static __m128i load_swapped(const u8 *source) {
    __m128i mask = _mm_loadu_si128((const __m128i *)byte_swap_mask);
    return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)source), mask);
}

static void store_swapped(u8 *destination, __m128i value) {
    __m128i mask = _mm_loadu_si128((const __m128i *)byte_swap_mask);
    _mm_storeu_si128((__m128i *)destination, _mm_shuffle_epi8(value, mask));
}

// Carry-less multiply of two 128-bit values, then reduce modulo the GCM
// polynomial x^128 + x^7 + x^2 + x + 1. The reduction is the shift-xor chain
// from the Intel carry-less multiplication white paper.
static __m128i gf_multiply(__m128i a, __m128i b) {
    __m128i low = _mm_clmulepi64_si128(a, b, 0x00);
    __m128i middle = _mm_xor_si128(_mm_clmulepi64_si128(a, b, 0x10),
                                   _mm_clmulepi64_si128(a, b, 0x01));
    __m128i high = _mm_clmulepi64_si128(a, b, 0x11);
    low = _mm_xor_si128(low, _mm_slli_si128(middle, 8));
    high = _mm_xor_si128(high, _mm_srli_si128(middle, 8));

    // GCM counts bits in the reverse order, so the 255-bit product has to be
    // shifted up by one before it can be reduced as an ordinary polynomial.
    __m128i low_carry = _mm_srli_epi32(low, 31);
    __m128i high_carry = _mm_srli_epi32(high, 31);
    low = _mm_slli_epi32(low, 1);
    high = _mm_slli_epi32(high, 1);
    __m128i spill = _mm_srli_si128(low_carry, 12);
    high_carry = _mm_slli_si128(high_carry, 4);
    low_carry = _mm_slli_si128(low_carry, 4);
    low = _mm_or_si128(low, low_carry);
    high = _mm_or_si128(_mm_or_si128(high, high_carry), spill);

    // Reduce modulo x^128 + x^7 + x^2 + x + 1.
    __m128i fold = _mm_xor_si128(_mm_xor_si128(_mm_slli_epi32(low, 31),
                                               _mm_slli_epi32(low, 30)),
                                 _mm_slli_epi32(low, 25));
    __m128i tail = _mm_srli_si128(fold, 4);
    fold = _mm_slli_si128(fold, 12);
    low = _mm_xor_si128(low, fold);
    __m128i reduced = _mm_xor_si128(_mm_xor_si128(_mm_srli_epi32(low, 1),
                                                  _mm_srli_epi32(low, 2)),
                                    _mm_srli_epi32(low, 7));
    reduced = _mm_xor_si128(reduced, tail);
    return _mm_xor_si128(high, _mm_xor_si128(low, reduced));
}

static __m128i ghash_blocks(__m128i accumulator, __m128i hash_key,
                            const u8 *data, u32 length) {
    u32 offset = 0;
    while (offset + AES_BLOCK_SIZE <= length) {
        accumulator = _mm_xor_si128(accumulator, load_swapped(data + offset));
        accumulator = gf_multiply(accumulator, hash_key);
        offset += AES_BLOCK_SIZE;
    }
    if (offset < length) {
        u8 partial[AES_BLOCK_SIZE];
        for (u32 index = 0; index < AES_BLOCK_SIZE; index++)
            partial[index] = offset + index < length ? data[offset + index] : 0;
        accumulator = _mm_xor_si128(accumulator, load_swapped(partial));
        accumulator = gf_multiply(accumulator, hash_key);
        crypto_zero(partial, sizeof(partial));
    }
    return accumulator;
}

static void counter_block(u8 block[AES_BLOCK_SIZE],
                          const u8 nonce[GCM_NONCE_SIZE], u32 counter) {
    for (u32 index = 0; index < GCM_NONCE_SIZE; index++)
        block[index] = nonce[index];
    block[12] = (u8)(counter >> 24);
    block[13] = (u8)(counter >> 16);
    block[14] = (u8)(counter >> 8);
    block[15] = (u8)counter;
}

void aes128_gcm_init(struct aes128_gcm *context,
                     const u8 key[AES128_KEY_SIZE]) {
    if (!context || !key) return;
    aes128_init(&context->cipher, key);
    u8 zero[AES_BLOCK_SIZE];
    for (u32 index = 0; index < AES_BLOCK_SIZE; index++) zero[index] = 0;
    aes128_encrypt_block(&context->cipher, zero, context->hash_key);
}

static void gcm_tag(const struct aes128_gcm *context,
                    const u8 nonce[GCM_NONCE_SIZE], const u8 *aad,
                    u32 aad_length, const u8 *ciphertext, u32 length,
                    u8 tag[GCM_TAG_SIZE]) {
    __m128i hash_key = load_swapped(context->hash_key);
    __m128i accumulator = _mm_setzero_si128();
    accumulator = ghash_blocks(accumulator, hash_key, aad, aad_length);
    accumulator = ghash_blocks(accumulator, hash_key, ciphertext, length);

    u8 lengths[AES_BLOCK_SIZE];
    u64 aad_bits = (u64)aad_length * 8ull;
    u64 text_bits = (u64)length * 8ull;
    for (u32 index = 0; index < 8; index++) {
        lengths[7 - index] = (u8)(aad_bits >> (index * 8));
        lengths[15 - index] = (u8)(text_bits >> (index * 8));
    }
    accumulator = _mm_xor_si128(accumulator, load_swapped(lengths));
    accumulator = gf_multiply(accumulator, hash_key);

    u8 hashed[AES_BLOCK_SIZE];
    store_swapped(hashed, accumulator);
    u8 first[AES_BLOCK_SIZE];
    u8 keystream[AES_BLOCK_SIZE];
    counter_block(first, nonce, 1);
    aes128_encrypt_block(&context->cipher, first, keystream);
    for (u32 index = 0; index < GCM_TAG_SIZE; index++)
        tag[index] = hashed[index] ^ keystream[index];
    crypto_zero(hashed, sizeof(hashed));
    crypto_zero(keystream, sizeof(keystream));
}

static void gcm_crypt(const struct aes128_gcm *context,
                      const u8 nonce[GCM_NONCE_SIZE], const u8 *input,
                      u32 length, u8 *output) {
    u8 block[AES_BLOCK_SIZE];
    u8 keystream[AES_BLOCK_SIZE];
    u32 offset = 0;
    // Counter 1 is reserved for the tag, so the payload starts at 2.
    u32 counter = 2;
    while (offset < length) {
        counter_block(block, nonce, counter);
        aes128_encrypt_block(&context->cipher, block, keystream);
        u32 remaining = length - offset;
        u32 take = remaining < AES_BLOCK_SIZE ? remaining : AES_BLOCK_SIZE;
        for (u32 index = 0; index < take; index++)
            output[offset + index] = input[offset + index] ^ keystream[index];
        offset += take;
        counter++;
    }
    crypto_zero(block, sizeof(block));
    crypto_zero(keystream, sizeof(keystream));
}

int aes128_gcm_seal(const struct aes128_gcm *context,
                    const u8 nonce[GCM_NONCE_SIZE], const void *aad,
                    u32 aad_length, const void *plaintext, u32 length,
                    u8 *ciphertext, u8 tag[GCM_TAG_SIZE]) {
    if (!context || !nonce || !tag) return -1;
    if ((!aad && aad_length) || (!plaintext && length) ||
        (!ciphertext && length))
        return -1;
    gcm_crypt(context, nonce, (const u8 *)plaintext, length, ciphertext);
    gcm_tag(context, nonce, (const u8 *)aad, aad_length, ciphertext, length,
            tag);
    return 0;
}

int aes128_gcm_open(const struct aes128_gcm *context,
                    const u8 nonce[GCM_NONCE_SIZE], const void *aad,
                    u32 aad_length, const void *ciphertext, u32 length,
                    const u8 tag[GCM_TAG_SIZE], u8 *plaintext) {
    if (!context || !nonce || !tag) return -1;
    if ((!aad && aad_length) || (!ciphertext && length) ||
        (!plaintext && length))
        return -1;
    u8 expected[GCM_TAG_SIZE];
    gcm_tag(context, nonce, (const u8 *)aad, aad_length,
            (const u8 *)ciphertext, length, expected);
    // The tag decides before any plaintext is produced. Decrypting first and
    // checking afterwards hands forged data to a caller that forgets to look
    // at the return value.
    int matched = crypto_equal(expected, tag, GCM_TAG_SIZE);
    crypto_zero(expected, sizeof(expected));
    if (!matched) return -1;
    gcm_crypt(context, nonce, (const u8 *)ciphertext, length, plaintext);
    return 0;
}
