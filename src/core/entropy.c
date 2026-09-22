#include "entropy.h"
#include "spinlock.h"

/* ChaCha20 DRBG (RFC 8439 block function, SP 800-90A-style usage): a
   bounded static state, reseeded after ENTROPY_RESEED_BYTES of output.
   Seed material prefers RDSEED, falls back to RDRAND, and always mixes
   the TSC-derived fallback so no single source (including an emulated
   RDRAND returning a constant) fully controls the key. */
#define ENTROPY_RESEED_BYTES (256u * 1024u)
#define ENTROPY_HW_RETRY 8u

struct entropy_state {
    struct spinlock lock;
    u32 initialized;
    u32 counter;
    u8 key[32];
    u8 nonce[12];
    u8 block[64];
    u32 block_used;
    u32 generated;
    int rdseed;
    int rdrand;
    u64 fallback_state;
};

static struct entropy_state entropy = { .lock = SPINLOCK_INIT };

static u32 load32(const u8 *source) {
    return (u32)source[0] | ((u32)source[1] << 8) |
           ((u32)source[2] << 16) | ((u32)source[3] << 24);
}

static void store32(u8 *destination, u32 value) {
    destination[0] = (u8)value;
    destination[1] = (u8)(value >> 8);
    destination[2] = (u8)(value >> 16);
    destination[3] = (u8)(value >> 24);
}

static u32 rotate_left(u32 value, u32 bits) {
    return (value << bits) | (value >> (32 - bits));
}

static void quarter_round(u32 *a, u32 *b, u32 *c, u32 *d) {
    *a += *b;
    *d ^= *a;
    *d = rotate_left(*d, 16);
    *c += *d;
    *b ^= *c;
    *b = rotate_left(*b, 12);
    *a += *b;
    *d ^= *a;
    *d = rotate_left(*d, 8);
    *c += *d;
    *b ^= *c;
    *b = rotate_left(*b, 7);
}

void entropy_chacha20_block(const u8 key[32], u32 counter,
                            const u8 nonce[12], u8 out[64]) {
    u32 state[16];
    u32 working[16];
    state[0] = 0x61707865u;
    state[1] = 0x3320646eu;
    state[2] = 0x79622d32u;
    state[3] = 0x6b206574u;
    for (u32 index = 0; index < 8; index++)
        state[4 + index] = load32(key + index * 4);
    state[12] = counter;
    state[13] = load32(nonce);
    state[14] = load32(nonce + 4);
    state[15] = load32(nonce + 8);
    for (u32 index = 0; index < 16; index++) working[index] = state[index];
    for (u32 round = 0; round < 10; round++) {
        quarter_round(&working[0], &working[4], &working[8], &working[12]);
        quarter_round(&working[1], &working[5], &working[9], &working[13]);
        quarter_round(&working[2], &working[6], &working[10], &working[14]);
        quarter_round(&working[3], &working[7], &working[11], &working[15]);
        quarter_round(&working[0], &working[5], &working[10], &working[15]);
        quarter_round(&working[1], &working[6], &working[11], &working[12]);
        quarter_round(&working[2], &working[7], &working[8], &working[13]);
        quarter_round(&working[3], &working[4], &working[9], &working[14]);
    }
    for (u32 index = 0; index < 16; index++)
        store32(out + index * 4, working[index] + state[index]);
}

static int rdseed64(u64 *value) {
    for (u32 attempt = 0; attempt < ENTROPY_HW_RETRY; attempt++) {
        u64 sample;
        u8 carried;
        __asm__ volatile("rdseed %0; setc %1"
                         : "=r"(sample), "=q"(carried) :: "cc");
        if (carried) {
            *value = sample;
            return 0;
        }
    }
    return -1;
}

static int rdrand64(u64 *value) {
    for (u32 attempt = 0; attempt < ENTROPY_HW_RETRY; attempt++) {
        u64 sample;
        u8 carried;
        __asm__ volatile("rdrand %0; setc %1"
                         : "=r"(sample), "=q"(carried) :: "cc");
        if (carried) {
            *value = sample;
            return 0;
        }
    }
    return -1;
}

/* Time-derived fallback: never the sole source, but it breaks the case
   where an emulator returns identical RDRAND streams on every boot. */
static u64 fallback64(void) {
    u32 low;
    u32 high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    u64 clock = ((u64)high << 32) | low;
    entropy.fallback_state ^= clock;
    entropy.fallback_state ^= entropy.fallback_state << 13;
    entropy.fallback_state ^= entropy.fallback_state >> 7;
    entropy.fallback_state ^= entropy.fallback_state << 17;
    return entropy.fallback_state;
}

static void seed_material(u8 out[44]) {
    u64 slots[5];
    for (u32 index = 0; index < 5; index++) {
        u64 sample = 0;
        if (entropy.rdseed && !rdseed64(&sample)) {
            /* Mixed on purpose: defense in depth against one source. */
            sample ^= fallback64();
        } else if (entropy.rdrand && !rdrand64(&sample)) {
            sample ^= fallback64();
        } else {
            sample = fallback64();
        }
        slots[index] = sample;
    }
    for (u32 index = 0; index < 5; index++) {
        store32(out + index * 8, (u32)slots[index]);
        store32(out + index * 8 + 4, (u32)(slots[index] >> 32));
    }
    store32(out + 40, (u32)slots[4]);
}

/* Folds fresh material into the key and immediately absorbs one output
   block back into it (SP 800-90A backtracking-resistance pattern). */
static void entropy_reseed_locked(void) {
    u8 material[44];
    seed_material(material);
    for (u32 index = 0; index < 32; index++) entropy.key[index] ^= material[index];
    for (u32 index = 0; index < 12; index++)
        entropy.nonce[index] ^= material[32 + index];
    u8 fold[64];
    entropy_chacha20_block(entropy.key, entropy.counter, entropy.nonce, fold);
    for (u32 index = 0; index < 32; index++) entropy.key[index] ^= fold[index];
    entropy.generated = 0;
}

void entropy_init(void) {
    spin_lock(&entropy.lock);
    if (entropy.initialized) {
        spin_unlock(&entropy.lock);
        return;
    }
    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1u), "c"(0u));
    entropy.rdrand = (ecx >> 30) & 1;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(7u), "c"(0u));
    if (eax >= 7) entropy.rdseed = (ebx >> 18) & 1;
    u8 material[44];
    seed_material(material);
    for (u32 index = 0; index < 32; index++) entropy.key[index] = material[index];
    for (u32 index = 0; index < 12; index++)
        entropy.nonce[index] = material[32 + index];
    entropy.counter = 1;
    entropy.block_used = 64;
    entropy.generated = 0;
    entropy.initialized = 1;
    spin_unlock(&entropy.lock);
}

int entropy_fill(void *buffer, u32 length) {
    if (!buffer || !length || length > ENTROPY_FILL_MAX) return -1;
    spin_lock(&entropy.lock);
    if (!entropy.initialized) {
        spin_unlock(&entropy.lock);
        return -1;
    }
    u8 *out = (u8 *)buffer;
    u32 done = 0;
    while (done < length) {
        if (entropy.block_used == 64) {
            if (entropy.generated >= ENTROPY_RESEED_BYTES)
                entropy_reseed_locked();
            entropy_chacha20_block(entropy.key, entropy.counter,
                                   entropy.nonce, entropy.block);
            entropy.counter++;
            entropy.generated += 64;
            entropy.block_used = 0;
        }
        u32 chunk = 64 - entropy.block_used;
        if (chunk > length - done) chunk = length - done;
        for (u32 index = 0; index < chunk; index++)
            out[done + index] = entropy.block[entropy.block_used + index];
        entropy.block_used += chunk;
        done += chunk;
    }
    spin_unlock(&entropy.lock);
    return 0;
}
