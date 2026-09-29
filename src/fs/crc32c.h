#ifndef CRC32C_H
#define CRC32C_H

#include "types.h"

// CRC32C (Castagnoli, reflected) with the standard 0xFFFFFFFF init and final
// xor, so crc32c("123456789", 9) == 0xE3069283. Used for AdytumFS on-disk
// integrity: it detects corruption, it is not a cryptographic hash.
u32 crc32c(const void *data, u32 length);
int crc32c_hardware_available(void);

#endif
