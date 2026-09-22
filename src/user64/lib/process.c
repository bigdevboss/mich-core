#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <mich/syscall.h>
#include <posix_abi.h>

static int result_int(long result) {
    if (result >= 0) return (int)result;
    errno = (int)-result;
    return -1;
}

pid_t fork(void) {
    return result_int(mich_syscall0(POSIX_SYSCALL_FORK));
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return result_int(mich_syscall3(POSIX_SYSCALL_EXECVE, (unsigned long)path,
                                    (unsigned long)argv,
                                    (unsigned long)envp));
}

void _exit(int code) {
    mich_syscall1(POSIX_SYSCALL_EXIT, (unsigned long)code);
    for (;;) __asm__ volatile("pause");
}

pid_t waitpid(pid_t pid, int *status, int options) {
    return result_int(mich_syscall3(POSIX_SYSCALL_WAITPID,
                                     (unsigned long)pid,
                                     (unsigned long)status,
                                     (unsigned long)options));
}

pid_t getpid(void) {
    return result_int(mich_syscall0(POSIX_SYSCALL_GETPID));
}

pid_t getppid(void) {
    return result_int(mich_syscall0(POSIX_SYSCALL_GETPPID));
}
