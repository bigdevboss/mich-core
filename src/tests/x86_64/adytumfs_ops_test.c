#include "types.h"
#include "object.h"
#include "block.h"
#include "adytumfs_format.h"

int test_adytumfs_ops64(void) {
    u32 objects = object_active_count();
    u32 devices = block_active_count();
    struct kernel_object *dev = block_create(512, 0);   // 64 filesystem blocks

    // A 4 KiB block is too big for a modest stack, and the test is single
    // threaded, so the scratch block is static.
    static u8 block[ADYTUMFS_BLOCK_SIZE];
    struct adytumfs_superblock super;
    int valid = dev && adytumfs_make(dev) == 0 &&
        adytumfs_block_read(dev, 0, block) == 0 &&
        adytumfs_super_unpack(&super, block) == 0;

    u64 root = super.root_inode;
    u64 etc = 0;
    u64 readme = 0;
    u64 config = 0;
    u64 got = 0;

    // Create a directory and a file under the root, then resolve them by path.
    valid = valid &&
        adytumfs_create_at(dev, &super, root, "etc", 3, ADYTUMFS_MODE_DIR | 0755u,
                           &etc) == 0 &&
        adytumfs_create_at(dev, &super, root, "readme", 6,
                           ADYTUMFS_MODE_REG | 0644u, &readme) == 0 &&
        adytumfs_path_resolve(dev, &super, "/etc", &got) == 0 && got == etc &&
        adytumfs_path_resolve(dev, &super, "/readme", &got) == 0 &&
        got == readme &&
        adytumfs_path_resolve(dev, &super, "/", &got) == 0 && got == root &&
        adytumfs_path_resolve(dev, &super, "/nope", &got) != 0;

    // Duplicate names are refused.
    u64 duplicate = 0;
    valid = valid &&
        adytumfs_create_at(dev, &super, root, "readme", 6,
                           ADYTUMFS_MODE_REG | 0644u, &duplicate) != 0;

    // A nested file resolves through its parent directory.
    valid = valid &&
        adytumfs_create_at(dev, &super, etc, "config", 6,
                           ADYTUMFS_MODE_REG | 0644u, &config) == 0 &&
        adytumfs_path_resolve(dev, &super, "/etc/config", &got) == 0 &&
        got == config;

    // Create, write, resolve, read back: the pieces work together.
    u64 moved = 0;
    u8 window[2];
    valid = valid &&
        adytumfs_file_write(dev, &super, readme, 0, "hi", 2, &moved) == 0 &&
        moved == 2 &&
        adytumfs_path_resolve(dev, &super, "/readme", &got) == 0 &&
        adytumfs_file_read(dev, &super, got, 0, window, 2, &moved) == 0 &&
        moved == 2 && window[0] == 'h' && window[1] == 'i';

    // Unlink the file; the path stops resolving and the sibling survives.
    valid = valid &&
        adytumfs_unlink_at(dev, &super, root, "readme", 6) == 0 &&
        adytumfs_path_resolve(dev, &super, "/readme", &got) != 0 &&
        adytumfs_path_resolve(dev, &super, "/etc/config", &got) == 0 &&
        // Unlinking a directory through this path is refused.
        adytumfs_unlink_at(dev, &super, root, "etc", 3) != 0;

    if (dev) object_release(dev);
    valid = valid && object_active_count() == objects &&
        block_active_count() == devices;
    return valid ? 0 : -1;
}
