#include "types.h"
#include "crc32c.h"
#include "adytumfs_format.h"

int test_crc32c64(void) {
    // Standard Castagnoli check values, independent of the CPU: an empty input
    // hashes to zero and "123456789" to 0xE3069283.
    static const u8 check[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    static const u8 flipped[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '8' };
    int valid = crc32c(0, 0) == 0u &&
        crc32c(check, 0) == 0u &&
        crc32c(check, 9) == 0xE3069283u &&
        crc32c(flipped, 9) != crc32c(check, 9);

    // Little-endian on-disk accessors round-trip and lay bytes low-first.
    u8 buffer[8];
    adytumfs_write_le16(buffer, 0xABCDu);
    valid = valid && buffer[0] == 0xCDu && buffer[1] == 0xABu &&
        adytumfs_read_le16(buffer) == 0xABCDu;
    adytumfs_write_le32(buffer, 0x11223344u);
    valid = valid && buffer[0] == 0x44u && buffer[3] == 0x11u &&
        adytumfs_read_le32(buffer) == 0x11223344u;
    adytumfs_write_le64(buffer, 0x1122334455667788ull);
    valid = valid && buffer[0] == 0x88u && buffer[7] == 0x11u &&
        adytumfs_read_le64(buffer) == 0x1122334455667788ull;

    return valid ? 0 : -1;
}
