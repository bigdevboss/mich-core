#include "tests64.h"
#include "bootinfo.h"
#include "test_report.h"
#include "serial64.h"

int tests64_run(const struct test64_env *env) {
    if (!env || !env->owner || !env->target || !env->target_result)
        return -1;
    if (test_report_record(TEST_ID_OBJECT, test_object64(env->owner, env->target))) return -1;
    serial64_write("Mich test64: kernel object layer pass\n");
    serial64_write("Mich test64: 24-bit handle generation stress pass\n");
    if (test_report_record(TEST_ID_VFS, test_vfs64())) return -1;
    serial64_write("Mich test64: VFS vnode and mount objects pass\n");
    serial64_write("Mich test64: VFS ramfs backend pass\n");
    serial64_write("Mich test64: VFS immutable bootfs pass\n");
    serial64_write("Mich test64: VFS mount traversal pass\n");
    serial64_write("Mich test64: firmware lookup service pass\n");
    serial64_write("Mich test64: VFS file lifetime pass\n");
    serial64_write("Mich test64: VFS unlink-open semantics pass\n");
    serial64_write("Mich test64: VFS append transaction pass\n");
    serial64_write("Mich test64: VFS absolute and relative paths pass\n");
    serial64_write("Mich test64: VFS root escape protection pass\n");
    serial64_write("Mich test64: VFS path component bounds pass\n");
    serial64_write("Mich test64: VFS path mutation stress pass\n");
    if (test_report_record(TEST_ID_VFS_PAGES, test_vfs_pages64()))
        return -1;
    serial64_write("Mich test64: VFS page-backed files pass\n");
    if (test_report_record(TEST_ID_POSIX_FD,
                           test_posix_fd64(env->owner, env->target)))
        return -1;
    serial64_write("Mich test64: POSIX FD/OFD substrate pass\n");
    serial64_write("Mich test64: POSIX FD lifecycle cleanup pass\n");
    // test_posix_profile64 asserts that the init module was already admitted
    // to the POSIX profile at boot, which only holds when it was spawned with
    // BD_MODULE_POSIX_PROFILE. The hardware and panic profiles start init
    // without it, so running the test there fails on a condition that is not
    // a defect.
    if (env->module_flags & BD_MODULE_POSIX_PROFILE) {
        if (test_report_record(TEST_ID_POSIX_PROFILE,
                               test_posix_profile64(env->owner, env->target)))
            return -1;
        serial64_write("Mich test64: POSIX profile cwd and authority pass\n");
    }
    if (test_report_record(TEST_ID_POSIX_VFS,
                           test_posix_vfs64(env->owner, env->target)))
        return -1;
    serial64_write("Mich test64: POSIX VFS authority and mode pass\n");
    if (test_report_record(TEST_ID_POSIX_PROCESS, test_posix_process64()))
        return -1;
    serial64_write("Mich test64: POSIX process stack layout pass\n");
    if (test_report_record(TEST_ID_BLOCK, test_block64())) return -1;
    serial64_write("Mich test64: block device objects pass\n");
    serial64_write("Mich test64: ramdisk read and write pass\n");
    serial64_write("Mich test64: block request generation pass\n");
    serial64_write("Mich test64: block bounds and revoke pass\n");
    serial64_write("Mich test64: deferred block service pass\n");
    if (test_report_record(TEST_ID_CACHE, test_cache64())) return -1;
    serial64_write("Mich test64: block page cache pass\n");
    if (test_report_record(TEST_ID_ENTROPY, test_entropy64())) return -1;
    serial64_write("Mich test64: ChaCha20 DRBG pass\n");
    if (test_report_record(TEST_ID_RTC, test_rtc64())) return -1;
    serial64_write("Mich test64: RTC civil date conversion pass\n");
    if (test_report_record(TEST_ID_SHA256, test_sha256_64())) return -1;
    serial64_write("Mich test64: SHA-256, HMAC and HKDF pass\n");
    if (test_report_record(TEST_ID_AES_GCM, test_aes_gcm64())) return -1;
    serial64_write("Mich test64: AES-128-GCM pass\n");
    if (test_report_record(TEST_ID_BLOCKFS, test_blockfs64())) return -1;
    serial64_write("Mich test64: blockfs format and mount pass\n");
    serial64_write("Mich test64: blockfs file io pass\n");
    serial64_write("Mich test64: blockfs busy unmount lifetime pass\n");
    serial64_write("Mich test64: blockfs stale vnode generation pass\n");
    if (test_report_record(TEST_ID_BLOCKFS_PAGES, test_blockfs_pages64()))
        return -1;
    serial64_write("Mich test64: blockfs page-backed write-back pass\n");
    if (test_report_record(TEST_ID_RESOURCE, test_resource64())) return -1;
    serial64_write("Mich test64: resource object layer pass\n");
    if (test_report_record(TEST_ID_PAGE, test_page64(env))) return -1;
    serial64_write("Mich test64: page and shared memory objects pass\n");
    if (test_report_record(TEST_ID_PAGE_GROW, test_page_grow64()))
        return -1;
    serial64_write("Mich test64: page grow and trim pass\n");
    if (test_report_record(TEST_ID_SG, test_sg64())) return -1;
    serial64_write("Mich test64: scatter-gather objects pass\n");
    serial64_write("Mich test64: scatter-gather rollback pass\n");
    if (test_report_record(TEST_ID_RING, test_ring64(env))) return -1;
    serial64_write("Mich test64: shared zero-copy rings pass\n");
    serial64_write("Mich test64: ring index validation pass\n");
    serial64_write("Mich test64: ring generation revoke pass\n");
    if (test_report_record(TEST_ID_ASYNC, test_async64(env))) return -1;
    serial64_write("Mich test64: completion objects pass\n");
    serial64_write("Mich test64: completion cancel and timeout pass\n");
    serial64_write("Mich test64: timer objects pass\n");
    serial64_write("Mich test64: wait-many pass\n");
    if (test_report_record(TEST_ID_FPU, test_fpu64())) return -1;
    serial64_write("Mich test64: FPU save/load cost measured\n");
    return 0;
}
