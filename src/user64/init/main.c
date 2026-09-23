#include <mich/syscall.h>
#include <mich/service.h>
#include <mich/capability.h>
#include <mich/ipc.h>
#include <mich/event.h>
#include <mich/bridge.h>
#include <mich/hardware.h>
#include <mich/driver.h>
#include <mich/memory.h>
#include <mich/sg.h>
#include <mich/ring.h>
#include <mich/completion.h>
#include <mich/timer.h>
#include <mich/wait.h>
#include <mich/net.h>
#include <mich/socket.h>
#include <mich/virtio.h>
#include <mich/vfs.h>
#include <mich/firmware.h>
#include <mich/block.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

typedef unsigned long long u64;

static void stop(void) {
    for (;;) __asm__ volatile("pause");
}

static void vfs_path_set(struct mich_vfs_path_request *request,
                         unsigned int start_handle, unsigned int type,
                         const char *path) {
    request->start_handle = start_handle;
    request->type = type;
    request->node_handle = 0;
    request->reserved = 0;
    unsigned int index = 0;
    while (index < MICH_VFS_PATH_MAX - 1 && path[index]) {
        request->path[index] = path[index];
        index++;
    }
    while (index < MICH_VFS_PATH_MAX) request->path[index++] = 0;
}

static int posix_user_test(void) {
    const char path[] = "/posix-user-file";
    const char directory[] = "/posix-user-dir";
    const char payload[] = "abc";
    char readback[4];
    char cwd[32];
    struct stat info;
    int descriptor = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor != 0) return -1;
    if (write(descriptor, payload, 3) != 3 || lseek(descriptor, 0, 0) != 0 ||
        read(descriptor, readback, 3) != 3 || readback[0] != 'a' ||
        readback[1] != 'b' || readback[2] != 'c') {
        mich_write("Mich x86_64: POSIX user IO FAIL\n");
        return -1;
    }
    if (fstat(descriptor, &info) || !S_ISREG(info.st_mode) ||
        (info.st_mode & 0777u) != 0600u || fcntl(descriptor, F_GETFD) !=
        FD_CLOEXEC || fcntl(descriptor, F_SETFD, 0) ||
        fcntl(descriptor, F_GETFD) != 0 || truncate(path, 1) ||
        fstat(descriptor, &info) || info.st_size != 1 || close(descriptor)) {
        mich_write("Mich x86_64: POSIX user stat FAIL\n");
        return -1;
    }
    descriptor = open(path, O_RDONLY);
    if (descriptor != 0 || dup(descriptor) != 1 || close(1) || close(descriptor)) {
        mich_write("Mich x86_64: POSIX user dup FAIL\n");
        return -1;
    }
    if (mkdir(directory, 0700) || chdir(directory) || !getcwd(cwd, sizeof(cwd)) ||
        cwd[0] != '/' || cwd[1] != 'p' || cwd[2] != 'o' || cwd[3] != 's' ||
        cwd[4] != 'i' || cwd[5] != 'x' || cwd[6] != '-' || cwd[7] != 'u' ||
        cwd[8] != 's' || cwd[9] != 'e' || cwd[10] != 'r' || cwd[11] != '-' ||
        cwd[12] != 'd' || cwd[13] != 'i' || cwd[14] != 'r' || cwd[15] ||
        chdir("/") || unlink(path) || rmdir(directory)) {
        mich_write("Mich x86_64: POSIX user path FAIL\n");
        return -1;
    }
    return 0;
}

static int posix_user_process_test(void) {
    const char directory[] = "/posix-proc";
    char *const empty_env[] = { 0 };
    char *const args_missing[] = { "/boot/absent-image", 0 };
    char *const args_fixture[] = { "/boot/posixapp", "alpha", "beta", 0 };
    char *const env_fixture[] = { "POSIXAPP=stage5", 0 };
    char *many[34];
    static char huge[2050];
    static const char payload[] = "mich-posix-stage5";
    char buffer[32];
    int status = -1;

    if (mkdir(directory, 0700)) return -1;
    /* Fixture contract: 0 sync flag, 1 payload, 2 CLOEXEC victim. */
    int sync_fd = open("/posix-proc/sync", O_RDWR | O_CREAT, 0600);
    int data_fd = open("/posix-proc/data", O_RDWR | O_CREAT, 0600);
    int gone_fd = open("/posix-proc/gone", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (sync_fd != 0 || data_fd != 1 || gone_fd != 2) return -1;
    if (write(sync_fd, "w", 1) != 1 ||
        write(data_fd, payload, sizeof(payload) - 1) !=
        (ssize_t)(sizeof(payload) - 1))
        return -1;
    if (chdir(directory)) return -1;

    errno = 0;
    if (execve("/boot/absent-image", args_missing, empty_env) != -1 ||
        errno != ENOENT)
        return -1;
    errno = 0;
    if (execve("/boot", args_fixture, empty_env) != -1 || errno != EISDIR)
        return -1;
    for (int index = 0; index < 33; index++) many[index] = "x";
    many[33] = 0;
    errno = 0;
    if (execve("/boot/posixapp", many, empty_env) != -1 || errno != E2BIG)
        return -1;
    for (int index = 0; index < 2049; index++) huge[index] = 'h';
    huge[2049] = 0;
    char *const args_huge[] = { huge, 0 };
    errno = 0;
    if (execve("/boot/posixapp", args_huge, empty_env) != -1 || errno != E2BIG)
        return -1;
    errno = 0;
    char **volatile bad_argv = (char **)(u64)1;
    if (execve("/boot/posixapp", bad_argv, empty_env) != -1 ||
        errno != EINVAL)
        return -1;
    /* Every failed execve above must leave descriptors and image intact. */
    if (lseek(data_fd, 0, 0) != 0 ||
        read(data_fd, buffer, sizeof(payload) - 1) !=
        (ssize_t)(sizeof(payload) - 1))
        return -1;
    if (fcntl(gone_fd, F_GETFD) != FD_CLOEXEC) return -1;

    if (getpid() != 1 || getppid() != 0) return -1;
    int child = fork();
    if (child < 0) return -1;
    if (!child) {
        execve("/boot/posixapp", args_fixture, env_fixture);
        _exit(9);
    }
    /* The child parks on the sync flag, so it cannot exit before this
       WNOHANG poll observes it alive. */
    if (waitpid(child, &status, WNOHANG) != 0) return -1;
    if (lseek(sync_fd, 0, 0) != 0 || write(sync_fd, "g", 1) != 1) return -1;
    if (waitpid(child, &status, 0) != child) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 7) return -1;
    errno = 0;
    if (waitpid(child, &status, 0) != -1 || errno != ECHILD) return -1;
    errno = 0;
    if (waitpid(9999, &status, 0) != -1 || errno != ECHILD) return -1;

    if (close(sync_fd) || close(data_fd) || close(gone_fd)) return -1;
    if (chdir("/") || unlink("/posix-proc/sync") ||
        unlink("/posix-proc/data") || unlink("/posix-proc/gone") ||
        rmdir(directory))
        return -1;
    return 0;
}


/* TEMPORARY OQ-1 REPRO - not part of any commit. */
__attribute__((noinline)) static void stress_putdec(u32 value) {
    char digits[12];
    u32 count = 0;
    do { digits[count++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (count) {
        char pair[2] = {digits[--count], 0};
        mich_write(pair);
    }
}

__attribute__((noinline)) static int stress_fd_rounds(void) {
    for (int round = 0; round < 2; round++) {
        for (;;) {
            int fd = open("/stress-fd", O_RDWR | O_CREAT | O_TRUNC, 0600);
            if (fd < 0) break;
        }
        for (int fd = 0; fd < 32; fd++) close(fd);
        unlink("/stress-fd");
    }
    return 0;
}

__attribute__((noinline)) static int fork_burst(int exit_code) {
    int count = 0;
    for (;;) {
        int child = fork();
        if (child < 0) break;
        if (!child) _exit(exit_code);
        count++;
    }
    for (;;) {
        int status;
        if (waitpid(-1, &status, 0) < 0) break;
    }
    return count;
}

__attribute__((noinline)) static int posix_stress_test(void) {
    if (stress_fd_rounds()) return -1;
    mich_write("Mich stress: fd exhaustion stable\n");
    int n = fork_burst(3);
    mich_write("Mich stress: burst=");
    stress_putdec((u32)n);
    mich_write("\n");
    return 0;
}

static int posix_application_test(void) {
    char *const demo_argv[] = { "/boot/posixdemo", "demo", 0 };
    char *const demo_envp[] = { "POSIXDEMO=stage6", 0 };
    int status = -1;
    int child = fork();
    if (child < 0) return -1;
    if (!child) {
        execve("/boot/posixdemo", demo_argv, demo_envp);
        _exit(9);
    }
    /* The application runs to completion without a sync contract, so a
       blocking wait is the honest rendezvous here. */
    if (waitpid(child, &status, 0) != child) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return 0;
}

int main(u64 role) {
    int pid = mich_getpid();
    if (role == 0) {
        mich_write("Mich x86_64: idle task alive\n");
        stop();
    }
    if (role == 3) return 33;
    if (role == 4) {
        if (!(mich_cap_get() & MICH_CAP_SERVICE_REGISTER)) return 1;
        if (mich_service_register(MICH_SERVICE_TEST) != 0) return 2;
        return 44;
    }
    if (role == 5) {
        struct mich_message message;
        if (mich_recv_from(1, &message) != 0) return 1;
        return message.type == 55 && message.data[0] == 0x5A ? 55 : 2;
    }
    if (role == 6) {
        struct mich_message message;
        message.from_id = 0;
        message.type = 66;
        for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) message.data[i] = 0;
        message.data[0] = 0x6A;
        return mich_send(1, &message) == 0 ? 66 : 3;
    }
    if (role == 7) {
        struct mich_message message;
        if (mich_recv(&message) != 0) return 4;
        return message.type == 77 ? 77 : 5;
    }
    if (role == 8) {
        __asm__ volatile("ud2");
        return 6;
    }
    if (role == 9) {
        for (;;) __asm__ volatile("pause");
    }
    if (role == 10) {
        struct mich_message message;
        message.from_id = 0;
        message.type = 10;
        for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) message.data[i] = 0;
        return mich_send(1, &message) < 0 ? 10 : 7;
    }
    if (role == 11) {
        int child = mich_spawn(12);
        return child > 0 ? 11 : 8;
    }
    if (role == 12) return 12;
    if (role == 13)
        return mich_kill(1) < 0 && mich_memfree() < 0 ? 13 : 9;
    if (role == 14) {
        __asm__ volatile("mov $1, %%rsp; mov $16, %%eax; syscall" :::
                         "rax", "rcx", "r11", "memory");
        for (;;) __asm__ volatile("pause");
    }
    if (role == 16) {
        unsigned int mxcsr;
        __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
        return mxcsr == 0x1F80 ? 16 : 4;
    }
    if (role == 17) {
        struct mich_message message;
        if (mich_recv_from(1, &message) != 0 || message.type != 17) return 5;
        unsigned int handle = (unsigned int)message.data[0] |
                              ((unsigned int)message.data[1] << 8) |
                              ((unsigned int)message.data[2] << 16) |
                              ((unsigned int)message.data[3] << 24);
        if (mich_event_wait(handle) != 0 || mich_handle_close(handle) != 0)
            return 6;
        return 17;
    }
    if (role == 18) {
        struct mich_message message;
        if (mich_recv_from(1, &message) != 0 || message.type != 18) return 7;
        for (unsigned int item = 0; item < 2; item++) {
            unsigned int offset = item * 4;
            unsigned int handle = (unsigned int)message.data[offset] |
                ((unsigned int)message.data[offset + 1] << 8) |
                ((unsigned int)message.data[offset + 2] << 16) |
                ((unsigned int)message.data[offset + 3] << 24);
            if (mich_event_wait(handle) != 0 || mich_handle_close(handle) != 0)
                return 8;
        }
        return 18;
    }
    if (role == 20) {
        struct mich_message message;
        if (mich_recv_from(1, &message) != 0 || message.type != 20) return 9;
        unsigned int handle = (unsigned int)message.data[0] |
            ((unsigned int)message.data[1] << 8) |
            ((unsigned int)message.data[2] << 16) |
            ((unsigned int)message.data[3] << 24);
        if (mich_mmio_map(handle, 0x110000000ULL) != 0) return 10;
        *(volatile unsigned int *)0x110000000ULL = 0x4D494348u;
        return 11;
    }
    if (role == 21 || role == 22) {
        struct mich_driver_bootstrap_info bootstrap;
        if (mich_driver_bootstrap(&bootstrap) != 0 ||
            bootstrap.abi_version != MICH_DRIVER_ABI_VERSION ||
            bootstrap.size != sizeof(bootstrap) ||
            bootstrap.pid != (unsigned int)pid || bootstrap.image_id != 0 ||
            bootstrap.resource_count != 2 ||
            bootstrap.resources[1].kind != MICH_DRIVER_RESOURCE_BRIDGE ||
            !bootstrap.resources[1].handle)
            stop();
        if (role == 21) {
            if (!bootstrap.generation || bootstrap.generation > 2 ||
                mich_cap_get() != MICH_CAP_SERVICE_REGISTER ||
                mich_service_register(MICH_SERVICE_TEST) != 0)
                stop();
            mich_write("Mich test64: driver live primary bootstrap pass\n");
            if (mich_bridge_wait(bootstrap.resources[1].handle) != 0) stop();
        } else {
            if (bootstrap.generation != 3 || mich_cap_get() != 0)
                stop();
            mich_write("Mich test64: driver live fallback bootstrap pass\n");
            mich_bridge_wait(bootstrap.resources[1].handle);
            stop();
        }
        __asm__ volatile("ud2");
        return (int)role;
    }
    if (role == 15) {
        int child = mich_spawn(9);
        if (child <= 0) return 1;
        struct mich_message message;
        message.from_id = 0;
        message.type = 15;
        for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) message.data[i] = 0;
        message.data[0] = child & 0xFF;
        message.data[1] = (child >> 8) & 0xFF;
        message.data[2] = (child >> 16) & 0xFF;
        message.data[3] = (child >> 24) & 0xFF;
        if (mich_send(1, &message) != 0) return 2;
        return mich_wait(child) == 137 ? 15 : 3;
    }
    if (role == 1) {
        struct mich_driver_bootstrap_info bootstrap;
        if (mich_driver_bootstrap(&bootstrap) == 0) stop();
        struct mich_firmware_open_request firmware;
        firmware.file_handle = 0;
        firmware.size = 0;
        firmware.reserved0 = 0;
        firmware.reserved1 = 0;
        for (unsigned int index = 0; index < MICH_FIRMWARE_NAME_MAX; index++)
            firmware.name[index] = 0;
        firmware.name[0] = 'i';
        firmware.name[1] = 'n';
        firmware.name[2] = 'i';
        firmware.name[3] = 't';
        firmware.name[4] = '6';
        firmware.name[5] = '4';
        if (mich_firmware_open(&firmware) == 0) stop();
        mich_write("Mich x86_64: driver bootstrap access pass\n");
        mich_write("Mich x86_64: firmware domain isolation pass\n");
    }
    volatile u64 marker;
    volatile unsigned int mxcsr;
    if (role == 2) {
        struct mich_vfs_path_request bootfs;
        vfs_path_set(&bootfs, 0, 0, "/boot/init64");
        if (mich_vfs_resolve(&bootfs) == 0) stop();
        if (mich_service_register(MICH_SERVICE_TEST) == 0) stop();
        marker = 0x22222222ULL;
        mxcsr = 0x5F80;
        __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
        mich_write("Mich x86_64: external init64 task two\n");
        mich_yield();
        __asm__ volatile("ud2");
        stop();
    }
    if (role != 1 || pid != 1) stop();
    if (mich_memfree() <= 128) stop();
    if (!(mich_cap_get() & MICH_CAP_SERVICE_REGISTER)) stop();
    if (mich_service_register(MICH_SERVICE_INIT) != 0) stop();
    if (mich_service_lookup(MICH_SERVICE_INIT) != pid) stop();
    marker = 0x11111111ULL;
    mxcsr = 0x3F80;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    mich_write("Mich x86_64: external init64 task one\n");
    mich_yield();
    if (marker != 0x11111111ULL) stop();
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    if (mxcsr != 0x3F80) stop();
    int free_before_wait = mich_memfree();
    if (mich_wait(-1) != 134) stop();
    if (mich_memfree() <= free_before_wait) stop();
    mich_write("Mich x86_64: E820 PMM pass\n");
    mich_write("Mich x86_64: wait any pass\n");
    mich_write("Mich x86_64: blocking wait pass\n");
    mich_write("Mich x86_64: VM reclaim pass\n");
    if (mich_exec("/no-such-image", 0) >= 0) stop();
    marker = 0x11111111ULL;
    int fork_child = mich_fork();
    if (fork_child == 0) {
        marker = 0x22222222ULL;
        mich_write("Mich x86_64: fork child alive\n");
        mich_exit(marker == 0x22222222ULL ? 42 : 1);
    }
    if (fork_child <= 0 || marker != 0x11111111ULL ||
        mich_wait(fork_child) != 42)
        stop();
    marker = 0x33333333ULL;
    if (marker != 0x33333333ULL) stop();
    int exec_child = mich_fork();
    if (exec_child == 0) {
        mich_exec("/boot/init64", 3);
        mich_exit(2);
    }
    if (exec_child <= 0 || mich_wait(exec_child) != 33) stop();
    mich_write("Mich x86_64: fork pass\n");
    mich_write("Mich x86_64: copy-on-write address isolation pass\n");
    mich_write("Mich x86_64: exec pass\n");
    int free_before_spawn = mich_memfree();
    int fpu_fresh = mich_spawn(16);
    if (fpu_fresh <= 0 || mich_wait(fpu_fresh) != 16) stop();
    int recycled = mich_spawn(3);
    if (recycled <= 0 || (recycled & 0xFFFF) != 2 || recycled == 2) stop();
    if (mich_wait(recycled) != 33) stop();
    if (mich_memfree() != free_before_spawn) stop();
    mich_write("Mich x86_64: fresh FPU state pass\n");
    mich_write("Mich x86_64: dynamic spawn pass\n");
    mich_write("Mich x86_64: PID generation pass\n");
    mich_write("Mich x86_64: resource reuse pass\n");
    int grant_free = mich_memfree();
    int delegated = mich_spawn(4);
    if (delegated <= 0) stop();
    if (mich_cap_grant(delegated, MICH_CAP_SERVICE_REGISTER) != 0) stop();
    if (mich_wait(delegated) != 44) stop();
    if (mich_service_lookup(MICH_SERVICE_TEST) >= 0) stop();
    if (mich_memfree() != grant_free) stop();
    mich_write("Mich x86_64: capability grant pass\n");
    mich_write("Mich x86_64: service cleanup pass\n");
    int lifecycle_free = mich_memfree();
    int orphan_parent = mich_spawn(11);
    if (orphan_parent <= 0 ||
        mich_cap_grant(orphan_parent, MICH_CAP_TASK_ADMIN) != 0)
        stop();
    if (mich_wait(orphan_parent) != 11) stop();
    if (mich_wait(-1) != 12) stop();
    int attacker = mich_spawn(13);
    if (attacker <= 0 || mich_wait(attacker) != 13) stop();
    int bad_return = mich_spawn(14);
    if (bad_return <= 0 || mich_wait(bad_return) != 141) stop();
    int waiting_parent = mich_spawn(15);
    if (waiting_parent <= 0 ||
        mich_cap_grant(waiting_parent, MICH_CAP_TASK_ADMIN) != 0)
        stop();
    struct mich_message kill_message;
    if (mich_recv_from((unsigned int)waiting_parent, &kill_message) != 0 ||
        kill_message.type != 15)
        stop();
    int waiting_child = (int)kill_message.data[0] |
                        ((int)kill_message.data[1] << 8) |
                        ((int)kill_message.data[2] << 16) |
                        ((int)kill_message.data[3] << 24);
    if (mich_kill(waiting_child) != 0 || mich_wait(waiting_parent) != 15)
        stop();
    if (mich_memfree() != lifecycle_free) stop();
    mich_write("Mich x86_64: reparenting pass\n");
    mich_write("Mich x86_64: orphan cleanup pass\n");
    mich_write("Mich x86_64: kill permission pass\n");
    mich_write("Mich x86_64: syscall return containment pass\n");
    mich_write("Mich x86_64: killed child wait wake pass\n");
    int ipc_free = mich_memfree();
    struct mich_message ipc_message;
    ipc_message.from_id = 0;
    ipc_message.type = 55;
    for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) ipc_message.data[i] = 0;
    ipc_message.data[0] = 0x5A;
    int ipc_receiver = mich_spawn(5);
    if (ipc_receiver <= 0 || mich_send((unsigned int)ipc_receiver, &ipc_message) != 0)
        stop();
    if (mich_wait(ipc_receiver) != 55) stop();
    int ipc_sender = mich_spawn(6);
    if (ipc_sender <= 0 || mich_recv_from((unsigned int)ipc_sender, &ipc_message) != 0)
        stop();
    if (ipc_message.type != 66 || ipc_message.data[0] != 0x6A) stop();
    if (mich_wait(ipc_sender) != 66) stop();
    int queued_one = mich_spawn(6);
    int queued_two = mich_spawn(6);
    if (queued_one <= 0 || queued_two <= 0) stop();
    mich_yield();
    if (mich_recv(&ipc_message) != 0 || ipc_message.type != 66) stop();
    if (mich_recv(&ipc_message) != 0 || ipc_message.type != 66) stop();
    if (mich_wait(queued_one) != 66 || mich_wait(queued_two) != 66) stop();
    int nb_receiver = mich_spawn(7);
    if (nb_receiver <= 0) stop();
    mich_yield();
    ipc_message.type = 77;
    if (mich_send_nb((unsigned int)nb_receiver, &ipc_message) != 0) stop();
    if (mich_wait(nb_receiver) != 77) stop();
    int dying = mich_spawn(8);
    if (dying <= 0 || mich_recv_from((unsigned int)dying, &ipc_message) != 0)
        stop();
    if (ipc_message.type != MICH_MSG_DIED ||
        ipc_message.from_id != (unsigned int)dying)
        stop();
    if (mich_wait(dying) != 134) stop();
    int cycle = mich_spawn(10);
    if (cycle <= 0) stop();
    ipc_message.type = 10;
    if (mich_send((unsigned int)cycle, &ipc_message) >= 0) stop();
    if (mich_wait(cycle) != 10) stop();
    int timeout_target = mich_spawn(9);
    if (timeout_target <= 0) stop();
    if (mich_send_timeout((unsigned int)timeout_target, &ipc_message, 2) != -110)
        stop();
    if (mich_kill(timeout_target) != 0 || mich_wait(timeout_target) != 137)
        stop();
    if (mich_send((unsigned int)recycled, &ipc_message) != -3) stop();
    if (mich_recv_from((unsigned int)mich_getpid(), &ipc_message) != -35) stop();
    if (mich_send_timeout(1, &ipc_message, 0x80000000U) != -22) stop();
    if (mich_recv((struct mich_message *)1) >= 0) stop();
    if (mich_memfree() != ipc_free) stop();
    mich_write("Mich x86_64: blocking IPC pass\n");
    mich_write("Mich x86_64: nonblocking IPC pass\n");
    mich_write("Mich x86_64: IPC sender queue pass\n");
    mich_write("Mich x86_64: IPC stale PID pass\n");
    mich_write("Mich x86_64: IPC deadlock pass\n");
    mich_write("Mich x86_64: IPC timeout pass\n");
    mich_write("Mich x86_64: IPC death notification pass\n");
    mich_write("Mich x86_64: IPC uaccess pass\n");
    int stress_free = mich_memfree();
    int parallel_one = mich_spawn(3);
    int parallel_two = mich_spawn(3);
    if (parallel_one <= 0 || parallel_two <= 0 ||
        mich_wait(parallel_one) != 33 || mich_wait(parallel_two) != 33)
        stop();
    for (int iteration = 0; iteration < 32; iteration++) {
        int child = mich_spawn(3);
        if (child <= 0 || mich_wait(child) != 33) stop();
    }
    if (mich_memfree() != stress_free) stop();
    mich_write("Mich x86_64: lifecycle stress pass\n");
    int auto_event = mich_event_create(MICH_EVENT_AUTO_RESET, 1);
    if (auto_event <= 0 || mich_event_wait((unsigned int)auto_event) != 0 ||
        mich_event_signal((unsigned int)auto_event) != 0 ||
        mich_event_wait((unsigned int)auto_event) != 0 ||
        mich_event_signal((unsigned int)auto_event) != 0 ||
        mich_event_reset((unsigned int)auto_event) != 0 ||
        mich_handle_close((unsigned int)auto_event) != 0 ||
        mich_event_wait((unsigned int)auto_event) >= 0)
        stop();
    int manual_event = mich_event_create(MICH_EVENT_MANUAL_RESET, 1);
    if (manual_event <= 0 ||
        mich_event_wait((unsigned int)manual_event) != 0 ||
        mich_event_wait((unsigned int)manual_event) != 0 ||
        mich_event_reset((unsigned int)manual_event) != 0 ||
        mich_event_signal((unsigned int)manual_event) != 0 ||
        mich_event_wait((unsigned int)manual_event) != 0 ||
        mich_handle_close((unsigned int)manual_event) != 0)
        stop();
    int timed_event = mich_event_create(MICH_EVENT_AUTO_RESET, 0);
    if (timed_event <= 0 ||
        mich_event_wait_timeout((unsigned int)timed_event, 2) != -110 ||
        mich_handle_close((unsigned int)timed_event) != 0)
        stop();
    int shared_event = mich_event_create(MICH_EVENT_AUTO_RESET, 1);
    int handle_child = mich_spawn(17);
    if (shared_event <= 0 || handle_child <= 0) stop();
    int remote_handle = mich_handle_duplicate(handle_child,
                                               (unsigned int)shared_event,
                                               MICH_RIGHT_WAIT);
    if (remote_handle <= 0 ||
        mich_handle_close((unsigned int)shared_event) != 0)
        stop();
    struct mich_message handle_message;
    handle_message.from_id = 0;
    handle_message.type = 17;
    for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) handle_message.data[i] = 0;
    handle_message.data[0] = remote_handle & 0xFF;
    handle_message.data[1] = (remote_handle >> 8) & 0xFF;
    handle_message.data[2] = (remote_handle >> 16) & 0xFF;
    handle_message.data[3] = (remote_handle >> 24) & 0xFF;
    if (mich_send((unsigned int)handle_child, &handle_message) != 0 ||
        mich_wait(handle_child) != 17)
        stop();
    int batch_child = mich_spawn(18);
    int batch_first = mich_event_create(MICH_EVENT_AUTO_RESET, 1);
    int batch_second = mich_event_create(MICH_EVENT_AUTO_RESET, 1);
    if (batch_child <= 0 || batch_first <= 0 || batch_second <= 0) stop();
    struct mich_handle_transfer transfers[2];
    transfers[0].source_handle = (unsigned int)batch_first;
    transfers[0].rights = MICH_RIGHT_WAIT;
    transfers[0].target_handle = 0;
    transfers[1].source_handle = (unsigned int)batch_second;
    transfers[1].rights = MICH_RIGHT_WAIT;
    transfers[1].target_handle = 0;
    if (mich_handle_transfer_batch(batch_child, transfers, 2) != 0 ||
        !transfers[0].target_handle || !transfers[1].target_handle ||
        mich_handle_close((unsigned int)batch_first) != 0 ||
        mich_handle_close((unsigned int)batch_second) != 0)
        stop();
    struct mich_message batch_message;
    batch_message.from_id = 0;
    batch_message.type = 18;
    for (int i = 0; i < MICH_MESSAGE_DATA_SIZE; i++) batch_message.data[i] = 0;
    unsigned int batch_handles[2] = {
        transfers[0].target_handle, transfers[1].target_handle
    };
    for (unsigned int item = 0; item < 2; item++) {
        unsigned int offset = item * 4;
        batch_message.data[offset] = batch_handles[item] & 0xFF;
        batch_message.data[offset + 1] = (batch_handles[item] >> 8) & 0xFF;
        batch_message.data[offset + 2] = (batch_handles[item] >> 16) & 0xFF;
        batch_message.data[offset + 3] = (batch_handles[item] >> 24) & 0xFF;
    }
    if (mich_send((unsigned int)batch_child, &batch_message) != 0 ||
        mich_wait(batch_child) != 18)
        stop();
    mich_write("Mich x86_64: event objects pass\n");
    mich_write("Mich x86_64: event timeout pass\n");
    mich_write("Mich x86_64: cross-process handle pass\n");
    mich_write("Mich x86_64: atomic handle batch pass\n");
    int bridge = mich_bridge_create();
    struct mich_bridge_notification bridge_notification;
    if (bridge <= 0 ||
        mich_bridge_read((unsigned int)bridge, &bridge_notification) >= 0 ||
        mich_handle_close((unsigned int)bridge) != 0)
        stop();
    int irq_handle = mich_irq_open(15);
    int irq_bridge = mich_bridge_create();
    if (irq_handle <= 0 || irq_bridge <= 0 ||
        mich_irq_bind((unsigned int)irq_handle,
                      (unsigned int)irq_bridge) != 0 ||
        mich_irq_unbind((unsigned int)irq_handle) != 0 ||
        mich_handle_close((unsigned int)irq_bridge) != 0 ||
        mich_handle_close((unsigned int)irq_handle) != 0)
        stop();
    mich_write("Mich x86_64: Driver Bridge userspace pass\n");
    mich_write("Mich x86_64: userspace IRQ handles pass\n");
    int pci_count = mich_pci_count();
    if (pci_count <= 0) stop();
    unsigned long long driver_virtual = 0x110000000ULL;
    int pci_handle = -1;
    int bar_handle = -1;
    long long bar_length = -1;
    for (int index = 0; index < pci_count && bar_handle < 0; index++) {
        int candidate = mich_pci_open((unsigned int)index);
        if (candidate < 0) continue;
        for (unsigned int bar = 0; bar < 6; bar++) {
            int mapped = mich_pci_bar_open((unsigned int)candidate, bar);
            if (mapped < 0) continue;
            long long length = mich_resource_length((unsigned int)mapped);
            if (length > 0 &&
                mich_mmio_map((unsigned int)mapped, driver_virtual) == 0) {
                pci_handle = candidate;
                bar_handle = mapped;
                bar_length = length;
                break;
            }
            mich_handle_close((unsigned int)mapped);
        }
        if (bar_handle < 0) mich_handle_close((unsigned int)candidate);
    }
    struct mich_irq_group_request invalid_group;
    invalid_group.resource_handle = (unsigned int)pci_handle;
    invalid_group.first = 0;
    invalid_group.count = 3;
    invalid_group.reserved = 0;
    for (unsigned int index = 0; index < MICH_IRQ_GROUP_MAX; index++)
        invalid_group.handles[index] = 0;
    if (pci_handle < 0 || bar_handle < 0 || bar_length <= 0 ||
        mich_msi_group_open(&invalid_group) == 0 ||
        mich_resource_unmap(driver_virtual,
                            (unsigned long long)bar_length) != 0)
        stop();
    int readonly_child = mich_spawn(20);
    int readonly_handle = readonly_child > 0 ?
        mich_handle_duplicate(readonly_child, (unsigned int)bar_handle,
                              MICH_RIGHT_READ | MICH_RIGHT_MAP) : -1;
    struct mich_message readonly_message;
    readonly_message.from_id = 0;
    readonly_message.type = 20;
    for (int index = 0; index < MICH_MESSAGE_DATA_SIZE; index++)
        readonly_message.data[index] = 0;
    readonly_message.data[0] = readonly_handle & 0xFF;
    readonly_message.data[1] = (readonly_handle >> 8) & 0xFF;
    readonly_message.data[2] = (readonly_handle >> 16) & 0xFF;
    readonly_message.data[3] = (readonly_handle >> 24) & 0xFF;
    if (readonly_child <= 0 || readonly_handle <= 0 ||
        mich_send((unsigned int)readonly_child, &readonly_message) != 0 ||
        mich_wait(readonly_child) != 142 ||
        mich_handle_close((unsigned int)bar_handle) != 0 ||
        mich_handle_close((unsigned int)pci_handle) != 0)
        stop();
    mich_write("Mich x86_64: read-only resource mapping pass\n");
    int dma_free = mich_memfree();
    int dma_handle = mich_dma_allocate(2, 0x3FFFFFFFULL);
    long long dma_length = dma_handle >= 0 ?
        mich_resource_length((unsigned int)dma_handle) : -1;
    if (dma_handle < 0 || dma_length != 8192 ||
        mich_dma_map((unsigned int)dma_handle, driver_virtual) != 0)
        stop();
    volatile unsigned long long *dma_memory =
        (volatile unsigned long long *)driver_virtual;
    dma_memory[0] = 0x4D494348444D4121ULL;
    dma_memory[1023] = 0x5245534F55524345ULL;
    if (dma_memory[0] != 0x4D494348444D4121ULL ||
        dma_memory[1023] != 0x5245534F55524345ULL ||
        mich_resource_unmap(driver_virtual, 8192) != 0 ||
        mich_handle_close((unsigned int)dma_handle) != 0 ||
        mich_memfree() != dma_free)
        stop();
    int page_free = mich_memfree();
    int page_handle = mich_page_create();
    int shared_handle = mich_shared_memory_create(2);
    if (page_handle <= 0 || shared_handle <= 0 ||
        mich_resource_length((unsigned int)page_handle) != 4096 ||
        mich_resource_length((unsigned int)shared_handle) != 8192 ||
        mich_page_pin((unsigned int)page_handle) != 0 ||
        mich_page_map((unsigned int)page_handle, driver_virtual) != 0)
        stop();
    volatile unsigned long long *page_memory =
        (volatile unsigned long long *)driver_virtual;
    page_memory[0] = 0x504147454F424A21ULL;
    if (page_memory[0] != 0x504147454F424A21ULL ||
        mich_resource_unmap(driver_virtual, 4096) != 0 ||
        mich_page_unpin((unsigned int)page_handle) != 0 ||
        mich_page_revoke((unsigned int)page_handle) != 0 ||
        mich_page_map((unsigned int)page_handle, driver_virtual) == 0 ||
        mich_handle_close((unsigned int)page_handle) != 0 ||
        mich_page_map((unsigned int)shared_handle, driver_virtual) != 0)
        stop();
    volatile unsigned long long *shared_memory =
        (volatile unsigned long long *)driver_virtual;
    shared_memory[0] = 0x5348415245442121ULL;
    shared_memory[1023] = 0x4D454D4F52592121ULL;
    struct mich_sg_create_request sg_request;
    sg_request.entry_count = 2;
    sg_request.reserved = 0;
    for (unsigned int index = 0; index < MICH_SG_ENTRY_MAX; index++) {
        sg_request.entries[index].page_handle = 0;
        sg_request.entries[index].page_index = 0;
        sg_request.entries[index].offset = 0;
        sg_request.entries[index].length = 0;
    }
    sg_request.entries[0].page_handle = (unsigned int)shared_handle;
    sg_request.entries[0].page_index = 0;
    sg_request.entries[0].offset = 128;
    sg_request.entries[0].length = 2048;
    sg_request.entries[1].page_handle = (unsigned int)shared_handle;
    sg_request.entries[1].page_index = 1;
    sg_request.entries[1].offset = 0;
    sg_request.entries[1].length = 2048;
    int sg_handle = mich_sg_create(&sg_request);
    if (shared_memory[0] != 0x5348415245442121ULL ||
        shared_memory[1023] != 0x4D454D4F52592121ULL || sg_handle <= 0 ||
        mich_resource_length((unsigned int)sg_handle) != 4096 ||
        mich_page_revoke((unsigned int)shared_handle) == 0 ||
        mich_sg_revoke((unsigned int)sg_handle) != 0 ||
        mich_handle_close((unsigned int)sg_handle) != 0 ||
        mich_handle_close((unsigned int)shared_handle) != 0 ||
        mich_memfree() != page_free)
        stop();
    int ring_free = mich_memfree();
    int ring_handle = mich_ring_create(8, 32);
    if (ring_handle <= 0 || mich_ring_create(3, 32) >= 0 ||
        mich_ring_create(8, 24) >= 0 ||
        mich_resource_length((unsigned int)ring_handle) != 4096 ||
        mich_ring_map((unsigned int)ring_handle, driver_virtual) != 0)
        stop();
    struct mich_ring_shared_header *ring_header =
        (struct mich_ring_shared_header *)driver_virtual;
    volatile unsigned long long *ring_descriptors =
        (volatile unsigned long long *)(driver_virtual +
                                       ring_header->data_offset);
    ring_descriptors[0] = 0x52494E4744455343ULL;
    ring_descriptors[4] = 0x5345434F4E442121ULL;
    ring_header->producer = ~0ULL;
    ring_header->consumer = ~0ULL;
    ring_header->generation = 0;
    ring_header->capacity = 0xFFFFFFFFu;
    if (mich_ring_submit((unsigned int)ring_handle, 4) != 0 ||
        ring_header->producer != 4 || ring_header->consumer != 0 ||
        ring_header->generation != 1 || ring_header->capacity != 8 ||
        ring_header->descriptor_size != 32 || ring_header->data_offset != 64 ||
        mich_ring_consume((unsigned int)ring_handle, 2) != 0 ||
        mich_ring_submit((unsigned int)ring_handle, 6) != 0 ||
        mich_ring_submit((unsigned int)ring_handle, 1) == 0 ||
        ring_header->producer != 10 || ring_header->consumer != 2 ||
        mich_ring_consume((unsigned int)ring_handle, 8) != 0 ||
        mich_ring_revoke((unsigned int)ring_handle) != 0 ||
        mich_handle_close((unsigned int)ring_handle) != 0 ||
        mich_memfree() != ring_free)
        stop();
    int async_free = mich_memfree();
    int completion = mich_completion_create();
    int timer = mich_timer_create();
    unsigned long long request_id = completion > 0 ?
        mich_completion_begin((unsigned int)completion, 0) : 0;
    struct mich_wait_many_request wait_request;
    wait_request.count = 2;
    wait_request.timeout = 5;
    for (unsigned int index = 0; index < MICH_WAIT_MANY_MAX; index++)
        wait_request.handles[index] = 0;
    wait_request.handles[0] = (unsigned int)completion;
    wait_request.handles[1] = (unsigned int)timer;
    if (completion <= 0 || timer <= 0 || !request_id ||
        mich_timer_arm((unsigned int)timer, 2, 0) != 0 ||
        mich_wait_many(&wait_request) != 1)
        stop();
    struct mich_completion_update update;
    update.id = request_id;
    update.status = 23;
    update.transferred = 4096;
    wait_request.timeout = 0;
    if (mich_completion_finish((unsigned int)completion, &update) != 0 ||
        mich_wait_many(&wait_request) != 0)
        stop();
    struct mich_completion_poll_result poll;
    poll.id = request_id;
    poll.status = 0;
    poll.transferred = 0;
    poll.state = 0;
    poll.reserved = 0;
    if (mich_completion_poll((unsigned int)completion, &poll) != 0 ||
        poll.status != 23 || poll.transferred != 4096 ||
        poll.state != MICH_COMPLETION_DONE)
        stop();
    unsigned long long canceled =
        mich_completion_begin((unsigned int)completion, 0);
    poll.id = canceled;
    poll.reserved = 0;
    if (!canceled || mich_completion_cancel((unsigned int)completion,
                                             canceled) != 0 ||
        mich_completion_poll((unsigned int)completion, &poll) != 0 ||
        poll.state != MICH_COMPLETION_CANCELED)
        stop();
    unsigned long long timed =
        mich_completion_begin((unsigned int)completion, 2);
    if (!timed || mich_completion_wait((unsigned int)completion) != 0)
        stop();
    poll.id = timed;
    poll.reserved = 0;
    if (mich_completion_poll((unsigned int)completion, &poll) != 0 ||
        poll.state != MICH_COMPLETION_TIMED_OUT || poll.status != -110 ||
        mich_timer_arm((unsigned int)timer, 1, 2) != 0 ||
        mich_timer_wait((unsigned int)timer) != 0 ||
        mich_timer_wait((unsigned int)timer) != 0 ||
        mich_timer_cancel((unsigned int)timer) != 0 ||
        mich_handle_close((unsigned int)timer) != 0 ||
        mich_handle_close((unsigned int)completion) != 0 ||
        mich_memfree() != async_free)
        stop();
    int net_free = mich_memfree();
    struct mich_vnic_create_request vnic_request;
    vnic_request.buffer_count = 16;
    vnic_request.ring_capacity = 8;
    vnic_request.vnic_handle = 0;
    vnic_request.pool_handle = 0;
    vnic_request.rx_ring_handle = 0;
    vnic_request.tx_ring_handle = 0;
    if (mich_vnic_create(&vnic_request) != 0 ||
        !vnic_request.vnic_handle || !vnic_request.pool_handle ||
        !vnic_request.rx_ring_handle || !vnic_request.tx_ring_handle ||
        mich_packet_pool_map(vnic_request.pool_handle, driver_virtual) != 0)
        stop();
    unsigned char test_frame[64];
    for (unsigned int index = 0; index < sizeof(test_frame); index++)
        test_frame[index] = (unsigned char)index;
    struct mich_net_packet_descriptor rx_descriptor;
    if (mich_vnic_inject(vnic_request.vnic_handle, test_frame,
                         sizeof(test_frame)) != 0 ||
        mich_vnic_receive(vnic_request.vnic_handle, &rx_descriptor) != 0 ||
        !rx_descriptor.buffer_id ||
        rx_descriptor.length != sizeof(test_frame))
        stop();
    unsigned int rx_slot = (unsigned int)rx_descriptor.buffer_id - 1;
    volatile unsigned char *pool_memory =
        (volatile unsigned char *)driver_virtual;
    for (unsigned int index = 0; index < sizeof(test_frame); index++)
        if (pool_memory[(unsigned long long)rx_slot * 4096 +
                        rx_descriptor.offset + index] != test_frame[index])
            stop();
    if (mich_vnic_release_rx(vnic_request.vnic_handle,
                             rx_descriptor.buffer_id) != 0)
        stop();
    unsigned long long tx_id =
        mich_vnic_acquire_tx(vnic_request.vnic_handle);
    if (!tx_id) stop();
    unsigned int tx_slot = (unsigned int)tx_id - 1;
    for (unsigned int index = 0; index < sizeof(test_frame); index++)
        pool_memory[(unsigned long long)tx_slot * 4096 +
                    NET_PACKET_HEADROOM + index] = test_frame[index];
    struct mich_vnic_frame_request tx_request;
    tx_request.buffer_id = tx_id;
    tx_request.user_address_low = 0;
    tx_request.user_address_high = 0;
    tx_request.offset = NET_PACKET_HEADROOM;
    tx_request.length = sizeof(test_frame);
    tx_request.flags = 0;
    struct mich_net_packet_descriptor tx_descriptor;
    struct mich_vnic_benchmark_request benchmark;
    benchmark.packets = 256;
    benchmark.batch_size = 8;
    benchmark.payload_size = sizeof(test_frame);
    benchmark.completed = 0;
    benchmark.cycles = 0;
    if (!tx_id || mich_vnic_submit_tx(vnic_request.vnic_handle,
                                       &tx_request) != 0 ||
        mich_vnic_drain_tx(vnic_request.vnic_handle, &tx_descriptor) != 0 ||
        tx_descriptor.buffer_id != tx_id ||
        mich_vnic_benchmark(vnic_request.vnic_handle, &benchmark) != 0 ||
        benchmark.completed != benchmark.packets || !benchmark.cycles ||
        mich_vnic_revoke(vnic_request.vnic_handle) != 0 ||
        mich_handle_close(vnic_request.rx_ring_handle) != 0 ||
        mich_handle_close(vnic_request.tx_ring_handle) != 0 ||
        mich_handle_close(vnic_request.pool_handle) != 0 ||
        mich_handle_close(vnic_request.vnic_handle) != 0 ||
        mich_memfree() != net_free)
        stop();
    int socket_free = mich_memfree();
    int sender_socket = mich_socket_create();
    int receiver_socket = mich_socket_create();
    struct mich_socket_bind_request bind_request;
    bind_request.address = 0x7F000001u;
    bind_request.port = 14000;
    bind_request.reserved = 1;
    if (sender_socket <= 0 || receiver_socket <= 0 ||
        mich_socket_bind((unsigned int)sender_socket, &bind_request) == 0)
        stop();
    bind_request.reserved = 0;
    if (mich_socket_bind((unsigned int)sender_socket, &bind_request) != 0)
        stop();
    bind_request.port = 14001;
    if (mich_socket_bind((unsigned int)receiver_socket, &bind_request) != 0)
        stop();
    static struct mich_socket_send_request send_request;
    send_request.destination_address = 0x7F000001u;
    send_request.destination_port = 14001;
    send_request.length = 32;
    for (unsigned int index = 0; index < MICH_SOCKET_PAYLOAD_MAX; index++)
        send_request.payload[index] = index < send_request.length
            ? (unsigned char)index : 0;
    static struct mich_socket_receive_result receive_result;
    send_request.length = MICH_SOCKET_PAYLOAD_MAX + 1;
    if (mich_socket_send_to((unsigned int)sender_socket, &send_request) == 0 ||
        mich_socket_receive_from((unsigned int)receiver_socket,
                                 &receive_result) == 0)
        stop();
    send_request.length = 32;
    send_request.destination_address = 0xC0A80101u;
    if (mich_socket_send_to((unsigned int)sender_socket, &send_request) == 0)
        stop();
    send_request.destination_address = 0x7F000001u;
    if (mich_socket_send_to((unsigned int)sender_socket, &send_request) != 0 ||
        mich_socket_wait((unsigned int)receiver_socket) != 0)
        stop();
    if (mich_socket_receive_from((unsigned int)receiver_socket,
                                 &receive_result) != 0 ||
        receive_result.source_address != 0x7F000001u ||
        receive_result.destination_address != 0x7F000001u ||
        receive_result.source_port != 14000 ||
        receive_result.destination_port != 14001 ||
        receive_result.length != 32)
        stop();
    for (unsigned int index = 0; index < receive_result.length; index++)
        if (receive_result.payload[index] != (unsigned char)index) stop();
    send_request.length = MICH_SOCKET_PAYLOAD_MAX;
    for (unsigned int index = 0; index < MICH_SOCKET_PAYLOAD_MAX; index++)
        send_request.payload[index] = (unsigned char)index;
    if (mich_socket_send_to((unsigned int)sender_socket, &send_request) != 0 ||
        mich_socket_wait((unsigned int)receiver_socket) != 0 ||
        mich_socket_receive_from((unsigned int)receiver_socket,
                                 &receive_result) != 0 ||
        receive_result.length != MICH_SOCKET_PAYLOAD_MAX ||
        receive_result.payload[0] != 0 ||
        receive_result.payload[MICH_SOCKET_PAYLOAD_MAX - 1] !=
            (unsigned char)(MICH_SOCKET_PAYLOAD_MAX - 1))
        stop();
    struct mich_wait_many_request socket_wait;
    socket_wait.count = 1;
    socket_wait.timeout = 2;
    for (unsigned int index = 0; index < MICH_WAIT_MANY_MAX; index++)
        socket_wait.handles[index] = 0;
    socket_wait.handles[0] = (unsigned int)receiver_socket;
    if (mich_wait_many(&socket_wait) != -110 ||
        mich_handle_close((unsigned int)sender_socket) != 0)
        stop();
    int replacement_socket = mich_socket_create();
    bind_request.port = 14000;
    if (replacement_socket <= 0 ||
        mich_socket_bind((unsigned int)replacement_socket, &bind_request) != 0 ||
        mich_handle_close((unsigned int)replacement_socket) != 0 ||
        mich_handle_close((unsigned int)receiver_socket) != 0)
        stop();
    int ephemeral_socket = mich_socket_create();
    bind_request.port = 0;
    if (ephemeral_socket <= 0 ||
        mich_socket_bind((unsigned int)ephemeral_socket, &bind_request) != 0 ||
        bind_request.port < MICH_SOCKET_EPHEMERAL_FIRST ||
        mich_handle_close((unsigned int)ephemeral_socket) != 0)
        stop();
    for (unsigned int index = 0; index < 64; index++) {
        int temporary_socket = mich_socket_create();
        bind_request.port = (unsigned short)(15000 + (index & 15));
        if (temporary_socket <= 0 ||
            mich_socket_bind((unsigned int)temporary_socket, &bind_request) != 0 ||
            mich_handle_close((unsigned int)temporary_socket) != 0)
            stop();
    }
    if (mich_memfree() != socket_free) stop();
    if (!(mich_cap_get() & MICH_CAP_VFS_ADMIN)) stop();
    int root_handle = mich_vfs_root();
    struct mich_vfs_name_request directory_request;
    directory_request.directory_handle = (unsigned int)root_handle;
    directory_request.type = MICH_VFS_NODE_DIRECTORY;
    directory_request.node_handle = 0;
    directory_request.reserved = 0;
    for (unsigned int index = 0; index < MICH_VFS_NAME_MAX; index++)
        directory_request.name[index] = 0;
    directory_request.name[0] = 't';
    directory_request.name[1] = 'm';
    directory_request.name[2] = 'p';
    if (root_handle <= 0 || mich_vfs_create(&directory_request) != 0)
        stop();
    struct mich_vfs_name_request file_request;
    file_request.directory_handle = directory_request.node_handle;
    file_request.type = MICH_VFS_NODE_REGULAR;
    file_request.node_handle = 0;
    file_request.reserved = 0;
    for (unsigned int index = 0; index < MICH_VFS_NAME_MAX; index++)
        file_request.name[index] = 0;
    file_request.name[0] = 'd';
    file_request.name[1] = 'a';
    file_request.name[2] = 't';
    file_request.name[3] = 'a';
    if (mich_vfs_create(&file_request) != 0) stop();
    struct mich_vfs_open_request open_request;
    open_request.node_handle = file_request.node_handle;
    open_request.rights = MICH_VFS_RIGHT_READ | MICH_VFS_RIGHT_WRITE;
    open_request.file_handle = 0;
    open_request.reserved = 0;
    if (mich_vfs_open(&open_request) != 0) stop();
    struct mich_vfs_io_request io_request;
    io_request.file_handle = open_request.file_handle;
    io_request.offset = 0;
    io_request.length = 64;
    io_request.transferred = 0;
    for (unsigned int index = 0; index < MICH_VFS_IO_MAX; index++)
        io_request.data[index] = index < 64 ? (unsigned char)index : 0;
    if (mich_vfs_write(&io_request) != 0 || io_request.transferred != 64)
        stop();
    for (unsigned int index = 0; index < MICH_VFS_IO_MAX; index++)
        io_request.data[index] = 0;
    io_request.transferred = 0;
    if (mich_vfs_read(&io_request) != 0 || io_request.transferred != 64)
        stop();
    for (unsigned int index = 0; index < 64; index++)
        if (io_request.data[index] != (unsigned char)index) stop();
    struct mich_vfs_stat_request stat_request;
    stat_request.handle = open_request.file_handle;
    stat_request.reserved = 0;
    if (mich_vfs_stat(&stat_request) != 0 ||
        stat_request.info.size != 64 || !stat_request.info.linked)
        stop();
    if (mich_vfs_unlink(&file_request) != 0 ||
        mich_vfs_stat(&stat_request) != 0 || stat_request.info.linked)
        stop();
    io_request.length = 8;
    io_request.transferred = 0;
    if (mich_vfs_read(&io_request) != 0 || io_request.transferred != 8 ||
        mich_handle_close(open_request.file_handle) != 0 ||
        mich_handle_close(file_request.node_handle) != 0 ||
        mich_vfs_unlink(&directory_request) != 0 ||
        mich_handle_close(directory_request.node_handle) != 0 ||
        mich_handle_close((unsigned int)root_handle) != 0)
        stop();
    struct mich_vfs_path_request path_request;
    vfs_path_set(&path_request, 0, MICH_VFS_NODE_DIRECTORY, "/var");
    if (mich_vfs_create_path(&path_request) != 0) stop();
    unsigned int var_handle = path_request.node_handle;
    vfs_path_set(&path_request, 0, MICH_VFS_NODE_DIRECTORY, "/var/lib");
    if (mich_vfs_create_path(&path_request) != 0) stop();
    unsigned int lib_handle = path_request.node_handle;
    vfs_path_set(&path_request, 0, MICH_VFS_NODE_DIRECTORY, "/var/lib/mich");
    if (mich_vfs_create_path(&path_request) != 0) stop();
    unsigned int mich_handle = path_request.node_handle;
    vfs_path_set(&path_request, 0, MICH_VFS_NODE_REGULAR,
                 "/var/lib/mich/state");
    if (mich_vfs_create_path(&path_request) != 0) stop();
    unsigned int state_handle = path_request.node_handle;
    vfs_path_set(&path_request, 0, 0,
                 "///var//lib/./mich/../mich/state");
    if (mich_vfs_resolve(&path_request) != 0 ||
        !path_request.node_handle)
        stop();
    unsigned int resolved_handle = path_request.node_handle;
    vfs_path_set(&path_request, mich_handle, 0, "../mich/state");
    if (mich_vfs_resolve(&path_request) != 0 ||
        !path_request.node_handle)
        stop();
    unsigned int relative_handle = path_request.node_handle;
    vfs_path_set(&path_request, 0, 0, "/../../var");
    if (mich_vfs_resolve(&path_request) != 0 ||
        !path_request.node_handle)
        stop();
    unsigned int clamped_handle = path_request.node_handle;
    if (mich_handle_close(resolved_handle) != 0 ||
        mich_handle_close(relative_handle) != 0 ||
        mich_handle_close(clamped_handle) != 0 ||
        mich_handle_close(state_handle) != 0)
        stop();
    vfs_path_set(&path_request, 0, 0, "/var/lib/mich/state");
    if (mich_vfs_unlink_path(&path_request) != 0)
        stop();
    vfs_path_set(&path_request, 0, 0, "/var/lib/mich");
    if (mich_vfs_unlink_path(&path_request) != 0 ||
        mich_handle_close(mich_handle) != 0)
        stop();
    vfs_path_set(&path_request, 0, 0, "/var/lib");
    if (mich_vfs_unlink_path(&path_request) != 0 ||
        mich_handle_close(lib_handle) != 0)
        stop();
    vfs_path_set(&path_request, 0, 0, "/var");
    if (mich_vfs_unlink_path(&path_request) != 0 ||
        mich_handle_close(var_handle) != 0)
        stop();
    vfs_path_set(&path_request, 0, 0, "/boot/init64");
    if (mich_vfs_resolve(&path_request) != 0 || !path_request.node_handle)
        stop();
    unsigned int boot_node_handle = path_request.node_handle;
    open_request.node_handle = boot_node_handle;
    open_request.rights = MICH_VFS_RIGHT_READ;
    open_request.file_handle = 0;
    open_request.reserved = 0;
    if (mich_vfs_open(&open_request) != 0) stop();
    io_request.file_handle = open_request.file_handle;
    io_request.offset = 0;
    io_request.length = 4;
    io_request.transferred = 0;
    if (mich_vfs_read(&io_request) != 0 || io_request.transferred != 4 ||
        io_request.data[0] != 0x7F || io_request.data[1] != 'E' ||
        io_request.data[2] != 'L' || io_request.data[3] != 'F')
        stop();
    stat_request.handle = open_request.file_handle;
    stat_request.reserved = 0;
    if (mich_vfs_stat(&stat_request) != 0 ||
        stat_request.info.filesystem != MICH_VFS_FILESYSTEM_BOOTFS ||
        !stat_request.info.readonly)
        stop();
    struct mich_vfs_open_request writable_boot;
    writable_boot.node_handle = boot_node_handle;
    writable_boot.rights = MICH_VFS_RIGHT_WRITE;
    writable_boot.file_handle = 0;
    writable_boot.reserved = 0;
    vfs_path_set(&path_request, 0, 0, "/boot/init64");
    if (mich_vfs_open(&writable_boot) == 0 ||
        mich_vfs_unlink_path(&path_request) == 0 ||
        mich_handle_close(open_request.file_handle) != 0 ||
        mich_handle_close(boot_node_handle) != 0)
        stop();
    mich_write("Mich x86_64: userspace VFS objects pass\n");
    mich_write("Mich x86_64: userspace VFS read and write pass\n");
    mich_write("Mich x86_64: userspace VFS unlink-open pass\n");
    mich_write("Mich x86_64: userspace VFS path resolution pass\n");
    mich_write("Mich x86_64: userspace VFS root clamp pass\n");
    mich_write("Mich x86_64: userspace immutable bootfs pass\n");
    int block_handle = mich_block_create(16, 0);
    struct block_info geometry;
    static struct mich_block_io_request block_request;
    unsigned int bi;
    if (block_handle <= 0 ||
        mich_block_info(block_handle, &geometry) != 0 ||
        geometry.sector_size != MICH_BLOCK_SECTOR_SIZE ||
        geometry.sector_count != 16)
        stop();
    for (bi = 0; bi < sizeof(block_request); bi++)
        ((unsigned char *)&block_request)[bi] = 0;
    block_request.device_handle = (unsigned int)block_handle;
    block_request.op = MICH_BLOCK_OP_WRITE;
    block_request.lba = 3;
    block_request.sectors = 1;
    for (bi = 0; bi < MICH_BLOCK_SECTOR_SIZE; bi++)
        block_request.data[bi] = (unsigned char)(bi ^ 0xA5);
    if (mich_block_submit(&block_request) != 0 ||
        mich_block_collect(&block_request) != 0 ||
        block_request.status != 0)
        stop();
    block_request.op = MICH_BLOCK_OP_READ;
    block_request.lba = 3;
    block_request.sectors = 1;
    for (bi = 0; bi < MICH_BLOCK_SECTOR_SIZE; bi++)
        block_request.data[bi] = 0;
    if (mich_block_submit(&block_request) != 0 ||
        mich_block_collect(&block_request) != 0)
        stop();
    for (bi = 0; bi < MICH_BLOCK_SECTOR_SIZE; bi++)
        if (block_request.data[bi] != (unsigned char)(bi ^ 0xA5)) stop();
    if (mich_block_revoke(block_handle) != 0 ||
        mich_handle_close(block_handle) != 0)
        stop();
    block_handle = mich_block_create(8, MICH_BLOCK_FLAG_DEFER);
    if (block_handle <= 0) stop();
    for (bi = 0; bi < sizeof(block_request); bi++)
        ((unsigned char *)&block_request)[bi] = 0;
    block_request.device_handle = (unsigned int)block_handle;
    block_request.op = MICH_BLOCK_OP_WRITE;
    block_request.lba = 1;
    block_request.sectors = 1;
    for (bi = 0; bi < MICH_BLOCK_SECTOR_SIZE; bi++)
        block_request.data[bi] = (unsigned char)(bi ^ 0x5A);
    if (mich_block_submit(&block_request) != 0 ||
        mich_block_collect(&block_request) == 0 ||
        mich_block_service(block_handle) != 0 ||
        mich_block_collect(&block_request) != 0 ||
        block_request.status != 0)
        stop();
    block_request.op = MICH_BLOCK_OP_READ;
    for (bi = 0; bi < MICH_BLOCK_SECTOR_SIZE; bi++)
        block_request.data[bi] = 0;
    if (mich_block_submit(&block_request) != 0 ||
        mich_block_service(block_handle) != 0 ||
        mich_block_collect(&block_request) != 0)
        stop();
    for (bi = 0; bi < MICH_BLOCK_SECTOR_SIZE; bi++)
        if (block_request.data[bi] != (unsigned char)(bi ^ 0x5A)) stop();
    if (mich_block_revoke(block_handle) != 0 ||
        mich_handle_close(block_handle) != 0)
        stop();
    mich_write("Mich x86_64: userspace block device pass\n");
    mich_write("Mich x86_64: userspace deferred block io pass\n");
    {
        int pci_n = mich_pci_count();
        int virtio_pci = -1;
        int index;
        for (index = 0; index < pci_n; index++) {
            int candidate = mich_pci_open((unsigned int)index);
            long vendor;
            long did;
            if (candidate < 0) continue;
            vendor = mich_pci_config_read16((unsigned int)candidate, 0);
            did = mich_pci_config_read16((unsigned int)candidate, 2);
            if (vendor == 0x1AF4 && did == 0x1042) {
                virtio_pci = candidate;
                break;
            }
            mich_handle_close((unsigned int)candidate);
        }
        if (virtio_pci > 0) {
            int virtio_blk = mich_virtio_blk_open(virtio_pci);
            if (virtio_blk <= 0) stop();
            for (bi = 0; bi < sizeof(block_request); bi++)
                ((unsigned char *)&block_request)[bi] = 0;
            block_request.device_handle = (unsigned int)virtio_blk;
            block_request.op = MICH_BLOCK_OP_WRITE;
            block_request.lba = 4;
            block_request.sectors = 1;
            for (bi = 0; bi < MICH_BLOCK_SECTOR_SIZE; bi++)
                block_request.data[bi] = (unsigned char)(bi ^ 0x3C);
            if (mich_block_submit(&block_request) != 0)
                stop();
            for (bi = 0; mich_block_collect(&block_request) != 0; bi++) {
                mich_block_service(virtio_blk);
                if (bi > 1000000u) stop();
            }
            if (block_request.status != 0) stop();
            block_request.op = MICH_BLOCK_OP_READ;
            for (bi = 0; bi < MICH_BLOCK_SECTOR_SIZE; bi++)
                block_request.data[bi] = 0;
            if (mich_block_submit(&block_request) != 0)
                stop();
            for (bi = 0; mich_block_collect(&block_request) != 0; bi++) {
                mich_block_service(virtio_blk);
                if (bi > 1000000u) stop();
            }
            for (bi = 0; bi < MICH_BLOCK_SECTOR_SIZE; bi++)
                if (block_request.data[bi] != (unsigned char)(bi ^ 0x3C))
                    stop();
            if (mich_block_revoke(virtio_blk) != 0 ||
                mich_handle_close((unsigned int)virtio_blk) != 0 ||
                mich_handle_close((unsigned int)virtio_pci) != 0)
                stop();
            mich_write("Mich x86_64: userspace virtio-blk pass\n");
        }
    }
    {
        int pci_n = mich_pci_count();
        int nvme_pci = -1;
        int index;
        for (index = 0; index < pci_n; index++) {
            int candidate = mich_pci_open((unsigned int)index);
            long subclass;
            if (candidate < 0) continue;
            subclass = mich_pci_config_read16((unsigned int)candidate, 0x0A);
            if (subclass == 0x0108 &&
                mich_pci_config_read8((unsigned int)candidate, 0x09) == 2) {
                nvme_pci = candidate;
                break;
            }
            mich_handle_close((unsigned int)candidate);
        }
        if (nvme_pci > 0) {
            int nvme = mich_nvme_open(nvme_pci);
            if (nvme <= 0) stop();
            for (bi = 0; bi < sizeof(block_request); bi++)
                ((unsigned char *)&block_request)[bi] = 0;
            block_request.device_handle = (unsigned int)nvme;
            block_request.op = MICH_BLOCK_OP_WRITE;
            block_request.lba = 6;
            block_request.sectors = 1;
            for (bi = 0; bi < MICH_BLOCK_SECTOR_SIZE; bi++)
                block_request.data[bi] = (unsigned char)(bi ^ 0x99);
            if (mich_block_submit(&block_request) != 0) stop();
            for (bi = 0; mich_block_collect(&block_request) != 0; bi++) {
                mich_block_service(nvme);
                if (bi > 1000000u) stop();
            }
            block_request.op = MICH_BLOCK_OP_READ;
            for (bi = 0; bi < MICH_BLOCK_SECTOR_SIZE; bi++)
                block_request.data[bi] = 0;
            if (mich_block_submit(&block_request) != 0) stop();
            for (bi = 0; mich_block_collect(&block_request) != 0; bi++) {
                mich_block_service(nvme);
                if (bi > 1000000u) stop();
            }
            if (block_request.status != 0) stop();
            for (bi = 0; bi < MICH_BLOCK_SECTOR_SIZE; bi++)
                if (block_request.data[bi] != (unsigned char)(bi ^ 0x99))
                    stop();
            if (mich_block_revoke(nvme) != 0 ||
                mich_handle_close((unsigned int)nvme) != 0 ||
                mich_handle_close((unsigned int)nvme_pci) != 0)
                stop();
            mich_write("Mich x86_64: userspace nvme device pass\n");
        }
    }
    mich_write("Mich x86_64: userspace PCI handles pass\n");
    mich_write("Mich x86_64: userspace BAR mapping pass\n");
    mich_write("Mich x86_64: userspace DMA mapping pass\n");
    mich_write("Mich x86_64: userspace page objects pass\n");
    mich_write("Mich x86_64: shared memory revoke pass\n");
    mich_write("Mich x86_64: userspace scatter-gather pass\n");
    mich_write("Mich x86_64: userspace shared ring pass\n");
    mich_write("Mich x86_64: userspace ring corruption rejected\n");
    mich_write("Mich x86_64: userspace completion objects pass\n");
    mich_write("Mich x86_64: userspace timer objects pass\n");
    mich_write("Mich x86_64: userspace wait-many pass\n");
    mich_write("Mich x86_64: userspace packet pool pass\n");
    mich_write("Mich x86_64: userspace virtual NIC pass\n");
    mich_write("Mich x86_64: userspace packet benchmark pass\n");
    mich_write("Mich x86_64: userspace UDP sockets pass\n");
    mich_write("Mich x86_64: userspace full-MTU UDP pass\n");
    mich_write("Mich x86_64: userspace ephemeral port pass\n");
    mich_write("Mich x86_64: userspace socket wait timeout pass\n");
    mich_write("Mich x86_64: userspace socket cleanup stress pass\n");
    if (mich_cap_drop(MICH_CAP_SERVICE_REGISTER) != 0) stop();
    if (mich_cap_get() & MICH_CAP_SERVICE_REGISTER) stop();
    if (mich_service_register(MICH_SERVICE_TEST) == 0) stop();
    mich_write("Mich x86_64: service registry pass\n");
    mich_write("Mich x86_64: capability state pass\n");
    mich_write("Mich x86_64: process lifecycle pass\n");
    mich_write("Mich x86_64: task pool pass\n");
    mich_write("Mich x86_64: userspace runtime pass\n");
    mich_write("Mich x86_64: ELF64 load pass\n");
    mich_write("Mich x86_64: address spaces pass\n");
    mich_write("Mich x86_64: FPU context pass\n");
    mich_write("Mich x86_64: context switch pass\n");
    if (posix_user_test()) stop();
    mich_write("Mich x86_64: POSIX userspace facade pass\n");
    if (posix_user_process_test()) stop();
    mich_write("Mich x86_64: POSIX userspace process pass\n");
    if (posix_application_test()) stop();
    mich_write("Mich x86_64: POSIX static application pass\n");
    if (posix_stress_test()) stop();
    mich_syscall0(4);
    stop();
    return 0;
}
