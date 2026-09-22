#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <mich/syscall.h>

/* First compatibility-claim fixture (profile step 6): a real static POSIX
   application launched by init64 through fork/execve. The parent role
   exercises the v0 core API surface; the child role is reached by the
   application exec'ing itself with its own argv/envp. Contract: entry table
   empty, cwd "/", parent passes {"posixdemo","demo"}, {"POSIXDEMO=stage6"}. */

static int string_equals(const char *left, const char *right) {
    while (*left && *left == *right) {
        left++;
        right++;
    }
    return *left == *right;
}

static int wait_for_go(int sync_fd) {
    for (unsigned long attempt = 0; attempt < 100000u; attempt++) {
        char flag = 0;
        if (lseek(sync_fd, 0, 0) != 0) return -1;
        if (read(sync_fd, &flag, 1) != 1) return -1;
        if (flag == 'g') return 0;
        mich_yield();
    }
    return -1;
}

static int io_demo(void) {
    static const char payload[] = "mich static posix demo";
    char first[12];
    char second[12];
    int fd = open("/posixdemo-io", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd != 0) return -1;
    if (write(fd, payload, sizeof(payload) - 1) !=
        (ssize_t)(sizeof(payload) - 1))
        return -1;
    /* dup shares the open-file description, so the alias continues at the
       shared offset instead of restarting the file. */
    int alias = dup(fd);
    if (alias != 1) return -1;
    if (lseek(fd, 0, 0) != 0) return -1;
    if (read(fd, first, sizeof(payload) / 2) != (ssize_t)(sizeof(payload) / 2))
        return -1;
    if (read(alias, second, sizeof(payload) / 2) !=
        (ssize_t)(sizeof(payload) / 2))
        return -1;
    first[sizeof(payload) / 2] = 0;
    second[sizeof(payload) / 2] = 0;
    struct stat info;
    if (!string_equals(first, "mich static") ||
        !string_equals(second, " posix demo"))
        return -1;
    if (fstat(alias, &info) || !S_ISREG(info.st_mode) ||
        (info.st_mode & 0777u) != 0600u ||
        info.st_size != (off_t)(sizeof(payload) - 1))
        return -1;
    if (close(alias) || close(fd)) return -1;
    return unlink("/posixdemo-io") ? -1 : 0;
}

static int cwd_demo(void) {
    char buffer[32];
    if (mkdir("/posixdemo-dir", 0700)) return -1;
    if (chdir("/posixdemo-dir")) return -1;
    if (!getcwd(buffer, sizeof(buffer)) ||
        !string_equals(buffer, "/posixdemo-dir"))
        return -1;
    int fd = open("relative.txt", O_WRONLY | O_CREAT, 0600);
    if (fd != 0) return -1;
    if (write(fd, "relative", 8) != 8) return -1;
    if (close(fd)) return -1;
    if (chdir("/")) return -1;
    struct stat info;
    if (stat("/posixdemo-dir/relative.txt", &info) || !S_ISREG(info.st_mode))
        return -1;
    if (unlink("/posixdemo-dir/relative.txt")) return -1;
    return rmdir("/posixdemo-dir") ? -1 : 0;
}

static int errno_demo(void) {
    char buffer[4];
    int status = -1;
    errno = 0;
    if (open("/posixdemo-absent", O_RDONLY) != -1 || errno != ENOENT)
        return -1;
    errno = 0;
    if (open("/", O_RDONLY) != -1 || errno != EISDIR) return -1;
    errno = 0;
    if (read(9999, buffer, sizeof(buffer)) != -1 || errno != EBADF)
        return -1;
    errno = 0;
    if (waitpid(9999, &status, 0) != -1 || errno != ECHILD) return -1;
    return 0;
}

static int string_demo(void) {
    char scratch[12];
    if (memset(scratch, 'a', sizeof(scratch)) != scratch) return -1;
    if (memcmp(scratch, "aaaaaaaaaaaa", sizeof(scratch))) return -1;
    memcpy(scratch, "0123456789A", sizeof(scratch));
    if (memcmp(scratch, "0123456789A", sizeof(scratch))) return -1;
    /* Overlapping ranges: memmove must behave as if copied through a
       temporary buffer, unlike a plain forward memcpy. */
    memmove(scratch + 1, scratch, 5);
    if (memcmp(scratch, "0012346789A", 11)) return -1;
    if (strlen("posixdemo") != 9) return -1;
    if (strcmp("abc", "abc") || strcmp("abc", "abd") >= 0 ||
        strcmp("b", "a") <= 0)
        return -1;
    if (strncmp("michabc", "michxyz", 4) ||
        !strncmp("michabc", "michxyz", 5))
        return -1;
    const char *hit = strchr("aXbXc", 'X');
    if (!hit || hit[0] != 'X' || hit[1] != 'b') return -1;
    if (strchr("abc", 'Z')) return -1;
    const char *last = strrchr("aXbXc", 'X');
    if (!last || last[1] != 'c') return -1;
    return 0;
}

static int stdio_demo(void) {
    char buffer[48];
    if (snprintf(buffer, sizeof(buffer), "mich %d", 42) != 7) return -1;
    if (strcmp(buffer, "mich 42")) return -1;
    if (snprintf(buffer, sizeof(buffer), "%08X|%-5d|%x", 48879, -7, 48879) != 19)
        return -1;
    if (strcmp(buffer, "0000BEEF|-7   |beef")) return -1;
    if (snprintf(buffer, sizeof(buffer), "%s/%c", "boot", 'p') != 6) return -1;
    if (strcmp(buffer, "boot/p")) return -1;
    if (snprintf(buffer, sizeof(buffer), "%p", (void *)0x1000) != 6) return -1;
    if (strcmp(buffer, "0x1000")) return -1;
    /* POSIX truncation contract: the full length is returned while the
       buffer stays bounded and nul-terminated. */
    if (snprintf(buffer, 4, "%s", "abcdefg") != 7) return -1;
    if (strcmp(buffer, "abc")) return -1;
    if (snprintf(buffer, sizeof(buffer), "%lu%%", 1000UL) != 5) return -1;
    if (strcmp(buffer, "1000%")) return -1;
    /* printf itself must stream through the bounded serial flush path. */
    if (printf("libc printf alive: %d %s 0x%x\n", 42, "mich", 48879) != 34)
        return -1;
    return 0;
}

static int heap_demo(void) {
    unsigned long base =
        (unsigned long)mich_syscall1(MICH_SYS_POSIX_BRK, 0);
    if (!base) return -1;
    /* Neighbor coalescing while the arena is still empty: two freed
       adjacent blocks must serve one larger request without growth. */
    unsigned char *first = malloc(64);
    unsigned char *second = malloc(64);
    unsigned char *third = malloc(64);
    if (!first || !second || !third) return -1;
    if (second - first != third - second) return -1;
    free(second);
    free(first);
    unsigned char *merged = malloc(120);
    if (!merged || merged != first) return -1;
    free(third);
    free(merged);
    /* A large request crosses page boundaries through the break syscall. */
    unsigned char *data = malloc(70000);
    if (!data) return -1;
    for (int index = 0; index < 70000; index++) data[index] = (unsigned char)index;
    for (int index = 0; index < 70000; index++)
        if (data[index] != (unsigned char)index) return -1;
    unsigned long grown =
        (unsigned long)mich_syscall1(MICH_SYS_POSIX_BRK, 0);
    if (grown <= base) return -1;
    /* Freed space is reused instead of growing the arena again. */
    free(data);
    unsigned char *reused = malloc(70000);
    if (!reused) return -1;
    if ((unsigned long)mich_syscall1(MICH_SYS_POSIX_BRK, 0) != grown) return -1;
    /* A double free must not merge the same block twice. */
    free(reused);
    free(reused);
    unsigned char *alive = malloc(32);
    if (!alive) return -1;
    alive[0] = 1;
    if (alive[0] != 1) return -1;
    unsigned char *zeroed = calloc(16, 16);
    if (!zeroed) return -1;
    for (int index = 0; index < 256; index++)
        if (zeroed[index]) return -1;
    /* realloc preserves the old contents across a move. */
    unsigned char *small = malloc(16);
    if (!small) return -1;
    for (int index = 0; index < 16; index++) small[index] = (unsigned char)(index + 1);
    unsigned char *expanded = realloc(small, 200);
    if (!expanded) return -1;
    for (int index = 0; index < 16; index++)
        if (expanded[index] != (unsigned char)(index + 1)) return -1;
    free(expanded);
    free(zeroed);
    free(alive);
    /* The heap window is bounded, so an oversized request reports ENOMEM. */
    errno = 0;
    if (malloc(32 * 1024 * 1024)) return -1;
    if (errno != ENOMEM) return -1;
    /* The break is grow-only: requesting the base back changes nothing. */
    if ((unsigned long)mich_syscall1(MICH_SYS_POSIX_BRK, base) != grown) return -1;
    return 0;
}

static int file_demo(void) {
    char text[64];
    unsigned char block[256];
    unsigned char mirror[256];
    for (int index = 0; index < 256; index++)
        block[index] = (unsigned char)(index * 7);
    FILE *file = fopen("/libc-file", "w");
    if (!file) return -1;
    if (fputs("mich libc file stream\n", file) == EOF) return -1;
    if (fprintf(file, "%s=%d hex=%04X\n", "value", 42, 48879) < 0) return -1;
    if (fputc('Z', file) == EOF) return -1;
    /* 700 pattern bytes force the 512-byte buffer through a mid-stream
       flush while the file stays under the 4 KiB VFS cap. */
    if (fwrite(block, 1, 256, file) != 256) return -1;
    if (fwrite(block, 1, 256, file) != 256) return -1;
    if (fwrite(block, 1, 188, file) != 188) return -1;
    /* ftell must account for bytes still staged in the buffer. */
    if (ftell(file) != 741) return -1;
    if (fclose(file)) return -1;
    file = fopen("/libc-file", "r");
    if (!file) return -1;
    if (ftell(file) != 0) return -1;
    if (!fgets(text, sizeof(text), file)) return -1;
    if (!string_equals(text, "mich libc file stream\n")) return -1;
    if (!fgets(text, sizeof(text), file)) return -1;
    if (!string_equals(text, "value=42 hex=BEEF\n")) return -1;
    if (fseek(file, 40, SEEK_SET)) return -1;
    if (fgetc(file) != 'Z') return -1;
    if (ftell(file) != 41) return -1;
    rewind(file);
    if (ftell(file) != 0) return -1;
    if (fseek(file, -700, SEEK_END)) return -1;
    if (ftell(file) != 41) return -1;
    for (int pass = 0; pass < 2; pass++) {
        if (fread(mirror, 1, 256, file) != 256) return -1;
        for (int index = 0; index < 256; index++)
            if (mirror[index] != block[index]) return -1;
    }
    if (fread(mirror, 1, 188, file) != 188) return -1;
    for (int index = 0; index < 188; index++)
        if (mirror[index] != block[index]) return -1;
    /* The sticky EOF flag rises on the short read, not before it. */
    if (fread(mirror, 1, 256, file) != 0) return -1;
    if (!feof(file)) return -1;
    if (fgetc(file) != EOF) return -1;
    if (fclose(file)) return -1;
    file = fopen("/libc-file", "a");
    if (!file) return -1;
    if (fputs("tail", file) == EOF) return -1;
    if (fclose(file)) return -1;
    file = fopen("/libc-file", "r");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END)) return -1;
    if (ftell(file) != 745) return -1;
    if (fseek(file, -4, SEEK_END)) return -1;
    if (!fgets(text, sizeof(text), file)) return -1;
    if (!string_equals(text, "tail")) return -1;
    /* Writing into a read stream fails and sets the error flag without
       disturbing the pending reads. */
    if (fwrite("x", 1, 1, file) != 0) return -1;
    if (!ferror(file)) return -1;
    clearerr(file);
    if (ferror(file)) return -1;
    if (fclose(file)) return -1;
    return unlink("/libc-file") ? -1 : 0;
}

static int entropy_demo(void) {
    unsigned char first[256];
    unsigned char second[256];
    if (getrandom(first, sizeof(first), 0) != (ssize_t)sizeof(first)) return -1;
    /* A stuck or absent generator must not look like all-zero or all-one
       output, and the bit balance must sit near one half. */
    unsigned int set_bits = 0;
    unsigned int same_zero = 1;
    unsigned int same_one = 1;
    for (int index = 0; index < 256; index++) {
        if (first[index]) same_zero = 0;
        if (first[index] != 0xFF) same_one = 0;
        for (int bit = 0; bit < 8; bit++)
            if (first[index] & (1u << bit)) set_bits++;
    }
    if (same_zero || same_one) return -1;
    if (set_bits < 896 || set_bits > 1152) return -1;
    if (getrandom(second, sizeof(second), 0) != (ssize_t)sizeof(second)) return -1;
    if (!memcmp(first, second, sizeof(first))) return -1;
    /* /dev/urandom rides the same DRBG through the file facade. */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    unsigned char third[256];
    if (read(fd, third, sizeof(third)) != (ssize_t)sizeof(third)) return -1;
    if (read(fd, first, 64) != 64) return -1;
    if (!memcmp(third, second, sizeof(third))) return -1;
    if (close(fd)) return -1;
    /* Reserved flags and zero length are rejected with EINVAL. */
    errno = 0;
    if (getrandom(second, 16, 1) != -1 || errno != EINVAL) return -1;
    errno = 0;
    if (getrandom(second, 0, 0) != -1 || errno != EINVAL) return -1;
    return 0;
}

static int process_demo(void) {
    static const char payload[] = "posixdemo-child-payload";
    char *const child_argv[] = { "/posixdemo", "child", 0 };
    char *const child_envp[] = { "POSIXDEMO=child", 0 };
    int status = -1;
    /* Child contract: 0 sync flag, 1 payload, 2 CLOEXEC victim. */
    int sync_fd = open("/posixdemo-sync", O_RDWR | O_CREAT, 0600);
    int data_fd = open("/posixdemo-data", O_RDWR | O_CREAT, 0600);
    int gone_fd = open("/posixdemo-gone", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (sync_fd != 0 || data_fd != 1 || gone_fd != 2) return -1;
    if (write(sync_fd, "w", 1) != 1 ||
        write(data_fd, payload, sizeof(payload) - 1) !=
        (ssize_t)(sizeof(payload) - 1))
        return -1;
    int child = fork();
    if (child < 0) return -1;
    if (!child) {
        execve("/boot/posixdemo", child_argv, child_envp);
        _exit(9);
    }
    /* The child parks on the sync flag, so the WNOHANG poll cannot race. */
    if (waitpid(child, &status, WNOHANG) != 0) return -1;
    if (lseek(sync_fd, 0, 0) != 0 || write(sync_fd, "g", 1) != 1) return -1;
    if (waitpid(child, &status, 0) != child) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) return -1;
    if (close(sync_fd) || close(data_fd) || close(gone_fd)) return -1;
    if (unlink("/posixdemo-sync") || unlink("/posixdemo-data") ||
        unlink("/posixdemo-gone"))
        return -1;
    return 0;
}

static int child_role(void) {
    static const char payload[] = "posixdemo-child-payload";
    char buffer[32];
    char cwd[32];
    if (wait_for_go(0)) return 70;
    if (!getcwd(cwd, sizeof(cwd)) || !string_equals(cwd, "/")) return 71;
    if (getpid() == getppid()) return 72;
    if (lseek(1, 0, 0) != 0 ||
        read(1, buffer, sizeof(payload) - 1) != (ssize_t)(sizeof(payload) - 1))
        return 73;
    buffer[sizeof(payload) - 1] = 0;
    if (!string_equals(buffer, payload)) return 74;
    errno = 0;
    if (fcntl(2, F_GETFD) != -1 || errno != EBADF) return 75;
    return 42;
}

int main(int argc, char **argv) {
    if (argc == 2 && string_equals(argv[1], "child")) {
        if (!environ || !string_equals(environ[0], "POSIXDEMO=child") ||
            environ[1])
            return 76;
        return child_role();
    }
    if (argc != 2 || !string_equals(argv[0], "/boot/posixdemo") ||
        !string_equals(argv[1], "demo") || !environ ||
        !string_equals(environ[0], "POSIXDEMO=stage6") || environ[1])
        return 80;
    char cwd[32];
    if (!getcwd(cwd, sizeof(cwd)) || !string_equals(cwd, "/")) return 81;
    mich_write("Mich x86_64: POSIX application alive\n");
    if (io_demo()) return 82;
    mich_write("Mich x86_64: POSIX application IO pass\n");
    if (cwd_demo()) return 83;
    mich_write("Mich x86_64: POSIX application cwd pass\n");
    if (errno_demo()) return 84;
    mich_write("Mich x86_64: POSIX application errno pass\n");
    if (string_demo()) return 86;
    mich_write("Mich x86_64: POSIX libc string pass\n");
    if (stdio_demo()) return 87;
    mich_write("Mich x86_64: POSIX libc stdio pass\n");
    if (heap_demo()) return 88;
    mich_write("Mich x86_64: POSIX libc heap pass\n");
    if (file_demo()) return 89;
    mich_write("Mich x86_64: POSIX libc file pass\n");
    if (entropy_demo()) return 90;
    mich_write("Mich x86_64: POSIX entropy pass\n");
    if (process_demo()) return 85;
    mich_write("Mich x86_64: POSIX application process pass\n");
    return 0;
}
