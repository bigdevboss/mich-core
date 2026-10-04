#include "types.h"
#include "object.h"
#include "task.h"
#include "vfs.h"
#include "posix_fd.h"
#include "posix_profile.h"
#include "posix_vfs.h"
#include "posix_abi.h"
#include "tests64.h"
#include "rtc64.h"

int test_posix_vfs64(struct task *owner, struct task *child) {
    u32 objects = object_active_count();
    u32 nodes = vfs_node_active_count();
    u32 files = vfs_file_active_count();
    u32 descriptors = posix_fd_active_count();
    u32 ofds = posix_ofd_active_count();
    struct kernel_object *root = vfs_root();
    struct kernel_object *directory = root ?
        vfs_create(root, "posix-api", VFS_NODE_DIRECTORY) : 0;
    int denied = directory ? posix_vfs_open(
        child, "/posix-api/denied", POSIX_OPEN_RDWR | POSIX_OPEN_CREAT, 0600) : -1;
    int admitted = !posix_profile_admit(owner);
    int state = admitted ? posix_vfs_open(
        owner, "/posix-api/state", POSIX_OPEN_RDWR | POSIX_OPEN_CREAT |
        POSIX_OPEN_CLOEXEC, 0600) : -1;
    struct vfs_node_info info;
    u32 transferred = 0;
    u8 payload[3] = { 'a', 'b', 'c' };
    u8 received[3];
    u32 position = 0;
    int valid = root && directory && denied == POSIX_VFS_EACCES && admitted &&
        state == 0;
    valid &= !posix_vfs_stat_path(owner, "/posix-api/state", &info);
    valid &= info.type == VFS_NODE_REGULAR && info.mode == 0600 &&
        info.links == 1 && info.uid == 0 && info.gid == 0 && info.mtime;
    valid &= !posix_fd_get_cloexec(owner, state, &position) && position;
    valid &= !posix_fd_write(owner, state, payload, sizeof(payload),
                             &transferred) && transferred == sizeof(payload);
    valid &= !posix_vfs_truncate_path(owner, "/posix-api/state", 1);
    valid &= !posix_fd_stat(owner, state, &info) && info.size == 1;
    valid &= !posix_fd_seek(owner, state, 0, 0, &position);
    valid &= !posix_fd_read(owner, state, received, sizeof(received),
                            &transferred) && transferred == 1 &&
        received[0] == payload[0];
    valid &= posix_vfs_open(owner, "/posix-api/state", POSIX_OPEN_RDONLY |
                            POSIX_OPEN_TRUNC, 0) == POSIX_VFS_EINVAL;

    int readonly = posix_vfs_open(owner, "/posix-api/readonly",
                                  POSIX_OPEN_RDONLY | POSIX_OPEN_CREAT, 0400);
    valid &= readonly == 1;
    if (readonly >= 0) valid &= !posix_fd_close(owner, readonly);
    int append = posix_vfs_open(owner, "/posix-api/state", POSIX_OPEN_RDWR |
                                POSIX_OPEN_APPEND, 0);
    valid &= append == 1;
    valid &= !posix_fd_write(owner, append, payload + 1, 1, &transferred) &&
        transferred == 1;
    if (append >= 0) valid &= !posix_fd_close(owner, append);
    valid &= posix_vfs_open(owner, "/posix-api/readonly", POSIX_OPEN_WRONLY,
                            0) == POSIX_VFS_EACCES;
    valid &= posix_vfs_open(owner, "/boot/init64", POSIX_OPEN_WRONLY, 0) ==
        POSIX_VFS_EROFS;
    valid &= !posix_vfs_mkdir(owner, "/posix-api/dir", 0700);
    // Directory listing: open read-only, drain packed records, hit the end,
    // and refuse to open a directory for writing.
    int listing = posix_vfs_open(owner, "/posix-api", POSIX_OPEN_RDONLY, 0);
    valid &= listing >= 0;
    valid &= posix_vfs_open(owner, "/posix-api", POSIX_OPEN_WRONLY, 0) ==
        POSIX_VFS_EISDIR;
    u8 listing_buffer[POSIX_IO_MAX];
    u32 listing_seen_state = 0;
    u32 listing_seen_readonly = 0;
    u32 listing_seen_dir = 0;
    u32 listing_seen_other = 0;
    while (listing >= 0) {
        u32 listing_transferred = 0;
        int listed = posix_fd_getdents(owner, listing, listing_buffer,
                                       sizeof(listing_buffer),
                                       &listing_transferred);
        valid &= !listed;
        if (listed || !listing_transferred) break;
        u32 listing_walk = 0;
        while (listing_walk + 24 <= listing_transferred) {
            const u8 *record = listing_buffer + listing_walk;
            u32 record_length = (u32)record[16] | ((u32)record[17] << 8) |
                ((u32)record[18] << 16) | ((u32)record[19] << 24);
            u32 record_type = (u32)record[20] | ((u32)record[21] << 8) |
                ((u32)record[22] << 16) | ((u32)record[23] << 24);
            const char *record_name = (const char *)(record + 24);
            if (record_length < 26 || (record_length & 7) ||
                listing_walk + record_length > listing_transferred) {
                valid = 0;
                break;
            }
            if (record_name[0] == 's' && record_name[1] == 't' &&
                record_name[2] == 'a' && record_name[3] == 't' &&
                record_name[4] == 'e' && !record_name[5]) {
                listing_seen_state++;
                if (record_type != POSIX_DT_REG) valid = 0;
            } else if (record_name[0] == 'd' && record_name[1] == 'i' &&
                       record_name[2] == 'r' && !record_name[3]) {
                listing_seen_dir++;
                if (record_type != POSIX_DT_DIR) valid = 0;
            } else if (record_name[0] == 'r' && record_name[1] == 'e' &&
                       record_name[2] == 'a' && record_name[3] == 'd' &&
                       record_name[4] == 'o' && record_name[5] == 'n' &&
                       record_name[6] == 'l' && record_name[7] == 'y' &&
                       !record_name[8]) {
                listing_seen_readonly++;
                if (record_type != POSIX_DT_REG) valid = 0;
            } else {
                listing_seen_other++;
            }
            listing_walk += record_length;
        }
    }
    valid &= listing_seen_state == 1 && listing_seen_readonly == 1 &&
        listing_seen_dir == 1 && !listing_seen_other;
    if (listing >= 0) valid &= !posix_fd_close(owner, listing);
    // Permission columns: the owner column decides even where wider bits
    // would allow the request, search permission gates the walk into a
    // directory, and the umask masks both file and directory creates.
    valid &= posix_vfs_open(owner, "/posix-api/owner-only",
                            POSIX_OPEN_WRONLY | POSIX_OPEN_CREAT, 0006) ==
        POSIX_VFS_EACCES;
    valid &= posix_vfs_stat_path(owner, "/posix-api/owner-only", &info) ==
        POSIX_PROFILE_ENOENT;
    int masked = posix_vfs_open(owner, "/posix-api/masked",
                                POSIX_OPEN_RDWR | POSIX_OPEN_CREAT, 0666);
    valid &= masked >= 0;
    valid &= !posix_vfs_stat_path(owner, "/posix-api/masked", &info) &&
        info.mode == 0644u;
    if (masked >= 0) valid &= !posix_fd_close(owner, masked);
    valid &= !posix_vfs_unlink(owner, "/posix-api/masked");
    valid &= !posix_vfs_mkdir(owner, "/posix-api/masked-dir", 0777);
    valid &= !posix_vfs_stat_path(owner, "/posix-api/masked-dir", &info) &&
        info.mode == 0755u;
    valid &= !posix_vfs_rmdir(owner, "/posix-api/masked-dir");
    valid &= !posix_vfs_mkdir(owner, "/posix-api/searchable", 0644);
    valid &= posix_vfs_open(owner, "/posix-api/searchable/inside",
                            POSIX_OPEN_WRONLY | POSIX_OPEN_CREAT, 0600) ==
        POSIX_PROFILE_EACCES;
    valid &= posix_profile_chdir(owner, "/posix-api/searchable") ==
        POSIX_PROFILE_EACCES;
    valid &= !posix_vfs_rmdir(owner, "/posix-api/searchable");
    // Mode and ownership edits: chmod reshapes the columns, fchmod reaches
    // through a descriptor, chown moves the owner, and the owner-only rule
    // holds with no root override.
    valid &= !posix_vfs_mkdir(owner, "/posix-api/perm", 0755);
    int owned = posix_vfs_open(owner, "/posix-api/perm/file",
                               POSIX_OPEN_RDWR | POSIX_OPEN_CREAT, 0600);
    valid &= owned >= 0;
    valid &= !posix_vfs_chmod(owner, "/posix-api/perm/file", 0);
    valid &= posix_vfs_open(owner, "/posix-api/perm/file",
                            POSIX_OPEN_RDONLY, 0) == POSIX_VFS_EACCES;
    if (owned >= 0) valid &= !posix_fd_fchmod(owner, owned, 0400);
    valid &= !posix_vfs_chown(owner, "/posix-api/perm/file", 1, 1);
    valid &= !posix_vfs_stat_path(owner, "/posix-api/perm/file", &info) &&
        info.uid == 1 && info.gid == 1 && info.mode == 0400;
    valid &= posix_vfs_open(owner, "/posix-api/perm/file",
                            POSIX_OPEN_RDONLY, 0) == POSIX_VFS_EACCES;
    // access answers through the calling uid, so a 0400 file another uid
    // owns is closed to this one even though the file exists.
    valid &= !posix_vfs_access(owner, "/posix-api/perm/file",
                               POSIX_ACCESS_F_OK);
    valid &= posix_vfs_access(owner, "/posix-api/perm/file",
                              POSIX_ACCESS_R_OK) == POSIX_VFS_EACCES;
    valid &= posix_vfs_access(owner, "/posix-api/perm/file",
                              POSIX_ACCESS_X_OK) == POSIX_VFS_EACCES;
    valid &= posix_vfs_chmod(owner, "/posix-api/perm/file", 0600) ==
        POSIX_VFS_EPERM;
    valid &= !posix_vfs_chown(owner, "/posix-api/perm/file", -1, 0);
    valid &= !posix_vfs_stat_path(owner, "/posix-api/perm/file", &info) &&
        info.uid == 1 && info.gid == 0;
    valid &= !posix_vfs_chown(owner, "/posix-api/perm/file", 0, 0);
    valid &= !posix_vfs_chmod(owner, "/posix-api/perm/file", 0600);
    valid &= !posix_vfs_stat_path(owner, "/posix-api/perm/file", &info) &&
        info.mode == 0600;
    // Back home the read and write columns open, the execute one stays
    // closed until an x bit exists, and the directory answers for the
    // columns a lookup or a create would need.
    valid &= !posix_vfs_access(owner, "/posix-api/perm/file",
                               POSIX_ACCESS_R_OK | POSIX_ACCESS_W_OK);
    valid &= posix_vfs_access(owner, "/posix-api/perm/file",
                              POSIX_ACCESS_X_OK) == POSIX_VFS_EACCES;
    valid &= !posix_vfs_chmod(owner, "/posix-api/perm/file", 0700);
    valid &= !posix_vfs_access(owner, "/posix-api/perm/file",
                               POSIX_ACCESS_X_OK);
    valid &= !posix_vfs_access(owner, "/posix-api/perm",
                               POSIX_ACCESS_R_OK | POSIX_ACCESS_W_OK |
                               POSIX_ACCESS_X_OK);
    valid &= posix_vfs_access(owner, "/posix-api/perm/missing",
                              POSIX_ACCESS_F_OK) == POSIX_PROFILE_ENOENT;
    valid &= posix_vfs_access(owner, "/posix-api/perm/file", 8u) ==
        POSIX_VFS_EINVAL;
    // Timestamp edits: chosen pairs land whole, OMIT leaves one field
    // standing, NOW samples the clock, and a bad nanosecond half is EINVAL
    // before any permissions are even asked.
    valid &= !posix_vfs_utimensat(owner, "/posix-api/perm/file",
                                  1000000000, 123456789, 1000000001,
                                  987654321);
    valid &= !posix_vfs_stat_path(owner, "/posix-api/perm/file", &info) &&
        info.atime == 1000000000 && info.atime_nsec == 123456789 &&
        info.mtime == 1000000001 && info.mtime_nsec == 987654321;
    valid &= !posix_vfs_utimensat(owner, "/posix-api/perm/file", 0,
                                  POSIX_UTIME_OMIT, 2000000000, 1);
    valid &= !posix_vfs_stat_path(owner, "/posix-api/perm/file", &info) &&
        info.atime == 1000000000 && info.atime_nsec == 123456789 &&
        info.mtime == 2000000000 && info.mtime_nsec == 1;
    // NOW samples the real wall clock, so it can land before the chosen
    // 2033 pair above: measure the instant and demand NOW lands at or
    // past it with a sane nanosecond half.
    u64 now_sec = rtc64_wall_clock();
    u32 now_nsec = rtc64_wall_clock_nsec();
    valid &= !posix_vfs_utimensat(owner, "/posix-api/perm/file", 0,
                                  POSIX_UTIME_NOW, 0, POSIX_UTIME_NOW);
    valid &= !posix_vfs_stat_path(owner, "/posix-api/perm/file", &info) &&
        (info.atime > now_sec ||
         (info.atime == now_sec && info.atime_nsec >= now_nsec)) &&
        info.atime_nsec <= 999999999u &&
        (info.mtime > now_sec ||
         (info.mtime == now_sec && info.mtime_nsec >= now_nsec)) &&
        info.mtime_nsec <= 999999999u;
    valid &= posix_vfs_utimensat(owner, "/posix-api/perm/file", 0,
                                 1000000000, 0, 0) == POSIX_VFS_EINVAL;
    valid &= posix_vfs_utimensat(owner, "/posix-api/perm/file", 0, -1,
                                 0, 0) == POSIX_VFS_EINVAL;
    valid &= posix_vfs_utimensat(owner, "/posix-api/perm/missing", 0,
                                 POSIX_UTIME_NOW, 0, POSIX_UTIME_NOW) ==
        POSIX_PROFILE_ENOENT;
    // The descriptor twin answers through the open file: chosen pairs from
    // the owner land, a bad half stays EINVAL, and a dead descriptor is
    // EBADF before the question of times even starts.
    valid &= !posix_fd_futimens(owner, owned, 1500000000, 42, 1500000001,
                                43);
    valid &= !posix_vfs_stat_path(owner, "/posix-api/perm/file", &info) &&
        info.atime == 1500000000 && info.atime_nsec == 42 &&
        info.mtime == 1500000001 && info.mtime_nsec == 43;
    valid &= posix_fd_futimens(owner, owned, 0, 0x7fffffff, 0, 0) ==
        POSIX_VFS_EINVAL;
    valid &= posix_fd_futimens(owner, 9, 0, POSIX_UTIME_NOW, 0,
                               POSIX_UTIME_NOW) == POSIX_VFS_EBADF;
    // A regular file in the middle of a path has no directory listing to
    // find the next name in, so the no-follow stat answers ENOTDIR the
    // same way the walk behind stat does.
    valid &= posix_vfs_lstat_path(owner, "/posix-api/perm/file/inside",
                                  &info) == POSIX_VFS_ENOTDIR;
    // Both OMIT is the sanctioned no-op: it changes nothing, so it asks no
    // permission question at all.
    valid &= !posix_vfs_utimensat(owner, "/posix-api/perm/file", 7,
                                  POSIX_UTIME_OMIT, 7, POSIX_UTIME_OMIT);
    valid &= !posix_vfs_stat_path(owner, "/posix-api/perm/file", &info) &&
        info.atime == 1500000000 && info.atime_nsec == 42 &&
        info.mtime == 1500000001 && info.mtime_nsec == 43;
    // A file another uid owns answers the POSIX ladder: chosen times need
    // the owner, NOW needs the owner or write permission.
    valid &= !posix_vfs_chown(owner, "/posix-api/perm/file", 1, 1);
    valid &= posix_vfs_utimensat(owner, "/posix-api/perm/file", 1, 0,
                                 1, 0) == POSIX_VFS_EPERM;
    valid &= posix_vfs_utimensat(owner, "/posix-api/perm/file", 0,
                                 POSIX_UTIME_NOW, 0, POSIX_UTIME_NOW) ==
        POSIX_VFS_EPERM;
    valid &= !posix_vfs_chown(owner, "/posix-api/perm/file", 0, 0);
    valid &= !posix_vfs_chmod(owner, "/posix-api/perm/file", 0666);
    valid &= !posix_vfs_chown(owner, "/posix-api/perm/file", 1, 1);
    valid &= posix_vfs_utimensat(owner, "/posix-api/perm/file", 1, 0,
                                 1, 0) == POSIX_VFS_EPERM;
    u64 moved_sec = rtc64_wall_clock();
    u32 moved_nsec = rtc64_wall_clock_nsec();
    valid &= !posix_vfs_utimensat(owner, "/posix-api/perm/file", 0,
                                  POSIX_UTIME_OMIT, 0, POSIX_UTIME_NOW);
    valid &= !posix_vfs_stat_path(owner, "/posix-api/perm/file", &info) &&
        info.atime == 1500000000 && info.atime_nsec == 42 &&
        (info.mtime > moved_sec ||
         (info.mtime == moved_sec && info.mtime_nsec >= moved_nsec)) &&
        info.mtime_nsec <= 999999999u;
    // A write-mode descriptor is the futimens ticket for NOW; a read-mode
    // one never carries chosen times or the clock.
    int writer = posix_vfs_open(owner, "/posix-api/perm/file",
                                POSIX_OPEN_WRONLY, 0);
    valid &= writer >= 0;
    if (writer >= 0) {
        valid &= !posix_fd_futimens(owner, writer, 0, POSIX_UTIME_NOW, 0,
                                    POSIX_UTIME_NOW);
        valid &= posix_fd_futimens(owner, writer, 5, 0, 5, 0) ==
            POSIX_VFS_EPERM;
        valid &= !posix_fd_close(owner, writer);
    }
    int reader = posix_vfs_open(owner, "/posix-api/perm/file",
                                POSIX_OPEN_RDONLY, 0);
    valid &= reader >= 0;
    if (reader >= 0) {
        valid &= posix_fd_futimens(owner, reader, 0, POSIX_UTIME_NOW, 0,
                                   POSIX_UTIME_NOW) == POSIX_VFS_EPERM;
        valid &= !posix_fd_close(owner, reader);
    }
    valid &= !posix_vfs_chown(owner, "/posix-api/perm/file", 0, 0);
    valid &= !posix_vfs_chmod(owner, "/posix-api/perm/file", 0700);
    if (owned >= 0) valid &= !posix_fd_close(owner, owned);
    valid &= !posix_vfs_unlink(owner, "/posix-api/perm/file");
    valid &= !posix_vfs_rmdir(owner, "/posix-api/perm");
    valid &= posix_vfs_chmod(owner, "/boot/init64", 0755) == POSIX_VFS_EROFS;
    valid &= posix_vfs_access(owner, "/boot/init64", POSIX_ACCESS_W_OK) ==
        POSIX_VFS_EROFS;
    valid &= !posix_vfs_mkdir(owner, "/posix-api/sealed", 0600);
    valid &= !posix_vfs_mkdir(owner, "/posix-api/no-write", 0500);
    valid &= posix_vfs_open(owner, "/posix-api/sealed/item",
                            POSIX_OPEN_WRONLY | POSIX_OPEN_CREAT, 0600) ==
        POSIX_VFS_EACCES;
    valid &= posix_vfs_open(owner, "/posix-api/no-write/item",
                            POSIX_OPEN_WRONLY | POSIX_OPEN_CREAT, 0600) ==
        POSIX_VFS_EACCES;
    valid &= posix_vfs_open(owner, "/posix-api/zero",
                            POSIX_OPEN_WRONLY | POSIX_OPEN_CREAT, 0000) ==
        POSIX_VFS_EACCES;
    valid &= posix_vfs_stat_path(owner, "/posix-api/zero", &info) ==
        POSIX_PROFILE_ENOENT;
    // A 240-character component fits the path budget and round-trips.
    char stretched[VFS_PATH_MAX];
    for (u32 index = 0; index < VFS_PATH_MAX; index++) stretched[index] = 0;
    const char *stretched_prefix = "/posix-api/";
    for (u32 index = 0; stretched_prefix[index]; index++)
        stretched[index] = stretched_prefix[index];
    for (u32 index = 0; index < 240; index++)
        stretched[11 + index] = 'n';
    int stretched_fd = posix_vfs_open(owner, stretched,
                                      POSIX_OPEN_WRONLY | POSIX_OPEN_CREAT,
                                      0600);
    valid &= stretched_fd >= 0;
    if (stretched_fd >= 0) valid &= !posix_fd_close(owner, stretched_fd);
    valid &= !posix_vfs_stat_path(owner, stretched, &info);
    valid &= !posix_vfs_unlink(owner, stretched);
    // Hard links: one inode under two names, the count follows the names,
    // and the rejects carry their POSIX errnos.
    int linked_data = posix_vfs_open(owner, "/posix-api/linked",
                                     POSIX_OPEN_WRONLY | POSIX_OPEN_CREAT,
                                     0600);
    valid &= linked_data >= 0;
    if (linked_data >= 0) valid &= !posix_fd_close(owner, linked_data);
    valid &= !posix_vfs_stat_path(owner, "/posix-api/linked", &info) &&
        info.links == 1;
    valid &= posix_vfs_link(owner, "/posix-api", "/posix-api/dir-twin") ==
        POSIX_VFS_EPERM;
    valid &= posix_vfs_link(owner, "/posix-api/linked", "/boot/file-twin") ==
        POSIX_VFS_EXDEV;
    valid &= posix_vfs_link(owner, "/posix-api/linked", "/posix-api") ==
        POSIX_VFS_EEXIST;
    valid &= !posix_vfs_link(owner, "/posix-api/linked",
                             "/posix-api/linked-twin");
    valid &= !posix_vfs_stat_path(owner, "/posix-api/linked-twin", &info) &&
        info.links == 2;
    valid &= !posix_vfs_stat_path(owner, "/posix-api/linked", &info) &&
        info.links == 2;
    valid &= !posix_vfs_unlink(owner, "/posix-api/linked");
    valid &= posix_vfs_stat_path(owner, "/posix-api/linked", &info) ==
        POSIX_PROFILE_ENOENT;
    valid &= !posix_vfs_stat_path(owner, "/posix-api/linked-twin", &info) &&
        info.links == 1;
    valid &= !posix_vfs_unlink(owner, "/posix-api/linked-twin");
    // The alias bound is the link count ceiling: the fifteenth extra name
    // lands and the sixteenth takes EMLINK.
    int bound_data = posix_vfs_open(owner, "/posix-api/bound",
                                    POSIX_OPEN_WRONLY | POSIX_OPEN_CREAT,
                                    0600);
    valid &= bound_data >= 0;
    if (bound_data >= 0) valid &= !posix_fd_close(owner, bound_data);
    static const char twin_prefix[] = "/posix-api/twin";
    char twin[sizeof(twin_prefix) + 2];
    for (u32 index = 0; index < VFS_ALIAS_MAX; index++) {
        for (u32 byte = 0; byte < sizeof(twin_prefix) - 1; byte++)
            twin[byte] = twin_prefix[byte];
        twin[sizeof(twin_prefix) - 1] = (char)('0' + index / 10);
        twin[sizeof(twin_prefix)] = (char)('0' + index % 10);
        twin[sizeof(twin_prefix) + 1] = 0;
        int twin_result = posix_vfs_link(owner, "/posix-api/bound", twin);
        valid &= index + 1u < VFS_ALIAS_MAX ? !twin_result :
            twin_result == POSIX_VFS_EMLINK;
    }
    valid &= !posix_vfs_stat_path(owner, "/posix-api/bound", &info) &&
        info.links == VFS_ALIAS_MAX;
    valid &= !posix_vfs_unlink(owner, "/posix-api/bound");
    for (u32 index = 0; index + 1u < VFS_ALIAS_MAX; index++) {
        for (u32 byte = 0; byte < sizeof(twin_prefix) - 1; byte++)
            twin[byte] = twin_prefix[byte];
        twin[sizeof(twin_prefix) - 1] = (char)('0' + index / 10);
        twin[sizeof(twin_prefix)] = (char)('0' + index % 10);
        twin[sizeof(twin_prefix) + 1] = 0;
        valid &= !posix_vfs_unlink(owner, twin);
    }
    valid &= posix_vfs_stat_path(owner, "/posix-api/bound", &info) ==
        POSIX_PROFILE_ENOENT;
    valid &= posix_vfs_unlink(owner, "/posix-api/dir") == POSIX_VFS_EISDIR;
    valid &= posix_vfs_rmdir(owner, "/posix-api/state") == POSIX_VFS_ENOTDIR;
    valid &= !posix_vfs_rmdir(owner, "/posix-api/dir");
    valid &= !posix_vfs_unlink(owner, "/posix-api/state");
    valid &= !posix_vfs_unlink(owner, "/posix-api/readonly");
    valid &= !posix_vfs_rmdir(owner, "/posix-api/sealed");
    valid &= !posix_vfs_rmdir(owner, "/posix-api/no-write");
    valid &= !posix_vfs_rmdir(owner, "/posix-api");

    if (state >= 0) valid &= !posix_fd_close(owner, state);
    posix_profile_release(owner);
    if (directory) object_release(directory);
    if (root) object_release(root);
    valid &= !posix_profile_admitted(owner) &&
        object_active_count() == objects && vfs_node_active_count() == nodes &&
        vfs_file_active_count() == files &&
        posix_fd_active_count() == descriptors && posix_ofd_active_count() == ofds;
    return valid ? 0 : -1;
}
