#include "types.h"
#include "object.h"
#include "resource.h"
#include "serial64.h"
#include "pci64.h"
#include "adytumfs.h"
#include "adytumfs_format.h"
#include "vfs.h"
#include "nvme.h"
#include "kernel64_internal.h"
#include "tests64.h"

// Boot workload for the crash harness. The harness boots this image against
// a persistent NVMe drive, cuts power at a chosen serial marker, and boots
// again: every boot first has to mount whatever the crash left behind, so
// the recovery check at the top is the actual assertion. A file must show
// one committed round or nothing; bytes that parse as neither are a torn
// commit. The rounds then rewrite both files and sync them, two commits the
// harness can cut between, before the next kill. A rename phase follows:
// one shuttle file moves between two names, so at most one of them exists
// at any moment and a cut inside the move may leave the inode nameless,
// an orphan the next mount still accepts.

#define CRASH_ROUNDS 24
#define CRASH_RENAME_ROUNDS 8
#define CRASH_PAYLOAD_MAGIC 0x6372617368646973u

static struct kernel_object *crash_nvme_open(void) {
    for (u32 index = 0; index < pci64_count(); index++) {
        struct kernel_object *candidate = pci64_object(index);
        const struct pci_resource *info = pci_resource_get(candidate);
        if (info && info->class_code == 0x01 && info->subclass == 0x08 &&
            info->programming_interface == 0x02)
            return nvme_open(candidate);
    }
    return 0;
}

static void crash_payload(u8 *buffer, u64 round) {
    adytumfs_write_le64(buffer, CRASH_PAYLOAD_MAGIC);
    adytumfs_write_le64(buffer + 8, round);
}

// Classify one file of the recovered pair. Zero means a committed round was
// found and its number is out; one means the file holds no committed round,
// which covers absent, empty, and a read the unsynced write never reached;
// minus one means bytes that are neither, which is a torn commit.
static int crash_round_read(struct kernel_object *disk, const char *name,
                            u64 *round) {
    struct kernel_object *node = vfs_lookup(disk, name);
    struct kernel_object *file = node ? vfs_open(node) : 0;
    u8 buffer[16];
    u32 moved = 0;
    int rc = file ?
        vfs_read(file, 0, buffer, sizeof(buffer), &moved) : -1;
    if (file) object_release(file);
    if (node) object_release(node);
    if (rc || moved == 0) return 1;
    if (moved != sizeof(buffer)) return -1;
    if (adytumfs_read_le64(buffer) != CRASH_PAYLOAD_MAGIC) return -1;
    *round = adytumfs_read_le64(buffer + 8);
    return 0;
}

int tests64_run_crash(void) {
    struct kernel_object *dev = crash_nvme_open();
    struct kernel_object *root = vfs_root();
    struct kernel_object *mnt = root ?
        vfs_create(root, "crashdisk", VFS_NODE_DIRECTORY) : 0;
    int valid = dev && mnt;
    if (valid && vfs_mount_adytumfs(mnt, dev)) {
        // A sound pair that will not mount is damage the harness must see,
        // not a blank drive to format over.
        struct adytumfs_superblock super;
        if (!adytumfs_super_read(dev, &super)) valid = 0;
        if (valid &&
            (adytumfs_format(dev) || vfs_mount_adytumfs(mnt, dev)))
            valid = 0;
        if (valid) serial64_write("Mich crash: volume formatted\n");
    }
    u64 recovered = 0;
    if (valid) {
        struct kernel_object *disk = vfs_lookup(root, "crashdisk");
        u64 left_round = 0;
        u64 right_round = 0;
        int left = disk ? crash_round_read(disk, "left", &left_round) : -1;
        int right = disk ? crash_round_read(disk, "right", &right_round) : -1;
        // The shuttle moves as one name, so at most one of its two names
        // can hold a committed round; bytes that parse as neither are a
        // torn commit either phase left behind.
        u64 shuttle_round = 0;
        u64 spare_round = 0;
        int shuttle = disk ?
            crash_round_read(disk, "shuttle-a", &shuttle_round) : -1;
        int spare = disk ?
            crash_round_read(disk, "shuttle-b", &spare_round) : -1;
        valid = disk && left >= 0 && right >= 0 && shuttle >= 0 &&
            spare >= 0 && (shuttle == 0) + (spare == 0) <= 1;
        if (valid) {
            recovered = left ? 0 : left_round;
            if (!right && right_round > recovered) recovered = right_round;
            serial64_write("Mich crash: recovery rounds ");
            serial64_hex(recovered);
            serial64_write("\n");
        }
        if (disk) object_release(disk);
    }
    struct kernel_object *disk = valid ? vfs_lookup(root, "crashdisk") : 0;
    struct kernel_object *left_node = disk ? vfs_lookup(disk, "left") : 0;
    struct kernel_object *right_node = disk ? vfs_lookup(disk, "right") : 0;
    if (disk && !left_node)
        left_node = vfs_create_mode(disk, "left", VFS_NODE_REGULAR, 0600);
    if (disk && !right_node)
        right_node = vfs_create_mode(disk, "right", VFS_NODE_REGULAR, 0600);
    struct kernel_object *left_file = left_node ? vfs_open(left_node) : 0;
    struct kernel_object *right_file = right_node ? vfs_open(right_node) : 0;
    // The shuttle lives under whichever of its two names survived, and a
    // boot that finds neither, the first one or one after a cut inside the
    // move, seeds it fresh with round zero so the invariant holds from the
    // very first recovery.
    struct kernel_object *shuttle_node = disk ?
        vfs_lookup(disk, "shuttle-a") : 0;
    if (disk && !shuttle_node) shuttle_node = vfs_lookup(disk, "shuttle-b");
    if (valid && disk && !shuttle_node)
        shuttle_node = vfs_create_mode(disk, "shuttle-a",
                                       VFS_NODE_REGULAR, 0600);
    struct kernel_object *shuttle_file = shuttle_node ?
        vfs_open(shuttle_node) : 0;
    valid = valid && disk && left_node && right_node && left_file &&
        right_file && shuttle_node && shuttle_file;
    u8 payload[16];
    if (valid && disk && !vfs_lookup(disk, "shuttle-a") &&
        !vfs_lookup(disk, "shuttle-b")) {
        crash_payload(payload, 0);
        u32 seeded = 0;
        valid = !vfs_write(shuttle_file, 0, payload, sizeof(payload),
                           &seeded) &&
            seeded == sizeof(payload) && !vfs_sync(shuttle_file);
    }
    for (u64 round = 1; valid && round <= CRASH_ROUNDS; round++) {
        crash_payload(payload, round);
        u32 moved = 0;
        serial64_write("Mich crash: point ");
        serial64_hex(round);
        serial64_write(" open\n");
        valid =
            !vfs_write(left_file, 0, payload, sizeof(payload), &moved) &&
            moved == sizeof(payload) &&
            !vfs_write(right_file, 0, payload, sizeof(payload), &moved) &&
            moved == sizeof(payload) &&
            !vfs_sync(left_file) && !vfs_sync(right_file);
        if (!valid) break;
        serial64_write("Mich crash: point ");
        serial64_hex(round);
        serial64_write(" committed\n");
        // A round is milliseconds of work while the harness polls the serial
        // log a few times a second, so pace the rounds to leave every kill
        // point standing long enough to be seen before the next one blurs
        // past it.
        for (u64 spin = 0; spin < 4000000u; spin++)
            __asm__ volatile("pause");
    }
    // The rename phase moves the shuttle between its two names with the
    // payload rewritten and committed first, so a cut can land inside the
    // payload commit, inside the rename window itself, or between phases;
    // at most one name survives any of them and the next boot picks the
    // survivor up where it stands.
    for (u64 phase = 1; valid && phase <= CRASH_RENAME_ROUNDS; phase++) {
        struct kernel_object *probe = vfs_lookup(disk, "shuttle-a");
        const char *from = probe ? "shuttle-a" : "shuttle-b";
        const char *to = probe ? "shuttle-b" : "shuttle-a";
        if (probe) object_release(probe);
        crash_payload(payload, 0x100u + phase);
        u32 moved = 0;
        valid = !vfs_write(shuttle_file, 0, payload, sizeof(payload),
                           &moved) &&
            moved == sizeof(payload) && !vfs_sync(shuttle_file);
        if (!valid) break;
        serial64_write("Mich crash: rename point ");
        serial64_hex(phase);
        serial64_write(" open\n");
        valid = !vfs_rename(disk, from, disk, to);
        if (!valid) break;
        serial64_write("Mich crash: rename point ");
        serial64_hex(phase);
        serial64_write(" committed\n");
        for (u64 spin = 0; spin < 4000000u; spin++)
            __asm__ volatile("pause");
    }
    if (valid) serial64_write("Mich crash: workload complete\n");
    if (left_file) object_release(left_file);
    if (right_file) object_release(right_file);
    if (shuttle_file) object_release(shuttle_file);
    if (left_node) object_release(left_node);
    if (right_node) object_release(right_node);
    if (shuttle_node) object_release(shuttle_node);
    if (disk) object_release(disk);
    if (valid && vfs_unmount(mnt)) valid = 0;
    if (mnt) object_release(mnt);
    if (root) object_release(root);
    if (dev) object_release(dev);
    return valid ? 0 : -1;
}
