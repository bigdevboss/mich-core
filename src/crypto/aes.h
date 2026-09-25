#ifndef MICH_CRYPTO_AES_H
#define MICH_CRYPTO_AES_H

#include "types.h"

#define AES_BLOCK_SIZE 16u
#define AES128_KEY_SIZE 16u
#define AES128_ROUNDS 10u

// Round keys as produced by aeskeygenassist. This build has no software AES:
// the cipher is the hardware instruction set or nothing, so that no key ever
// touches a lookup table whose access pattern a cache-timing attacker could
// read back.
struct aes128 {
    u8 round_keys[(AES128_ROUNDS + 1u) * AES_BLOCK_SIZE];
};

// 1 when the CPU reports both AES-NI and PCLMULQDQ. Everything else in this
// header and in gcm.h requires it: callers have to check once at startup and
// refuse to offer AES-GCM otherwise, because executing the instructions
// without support raises #UD.
int aes_hardware_available(void);

void aes128_init(struct aes128 *context, const u8 key[AES128_KEY_SIZE]);
void aes128_encrypt_block(const struct aes128 *context,
                          const u8 input[AES_BLOCK_SIZE],
                          u8 output[AES_BLOCK_SIZE]);

#endif
