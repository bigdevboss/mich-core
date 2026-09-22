#ifndef POSIX_PROCESS_H
#define POSIX_PROCESS_H

#include "types.h"

/* Bounded argv/envp contract of the POSIX process facade (profile 4.7). */
#define POSIX_ARG_COUNT_MAX 32u
#define POSIX_ARG_BYTES_MAX 2048u
#define POSIX_STACK_PAGE 4096u

#define POSIX_PROCESS_ENOENT (-2)
#define POSIX_PROCESS_E2BIG (-7)
#define POSIX_PROCESS_ECHILD (-10)
#define POSIX_PROCESS_EACCES (-13)
#define POSIX_PROCESS_EISDIR (-21)
#define POSIX_PROCESS_EINVAL (-22)

/* Strings live in one caller-owned arena; argv/envp point into it. */
struct posix_exec_vectors {
    u32 argc;
    u32 envc;
    const char *argv[POSIX_ARG_COUNT_MAX];
    const char *envp[POSIX_ARG_COUNT_MAX];
    u32 arena_bytes;
    const char *arena;
};

/* Builds the 4.7 initial stack image into stack_page (one page): argc,
   argv pointers plus NULL, envp pointers plus NULL, then the string arena.
   stack_rsp receives the 16-byte-aligned entry rsp. Pure function: no
   allocation, no user memory, safe to unit-test in ring 0. */
int posix_process_build_stack(u8 *stack_page,
                              const struct posix_exec_vectors *vectors,
                              u64 stack_base, u64 *stack_rsp);

#endif
