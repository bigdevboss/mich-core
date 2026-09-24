#include "crypto.h"

int crypto_equal(const void *left, const void *right, u32 length) {
    const u8 *a = (const u8 *)left;
    const u8 *b = (const u8 *)right;
    if (!a || !b) return 0;
    u8 difference = 0;
    for (u32 index = 0; index < length; index++) difference |= a[index] ^ b[index];
    return difference == 0;
}

void crypto_zero(void *buffer, u32 length) {
    volatile u8 *bytes = (volatile u8 *)buffer;
    if (!bytes) return;
    for (u32 index = 0; index < length; index++) bytes[index] = 0;
}
