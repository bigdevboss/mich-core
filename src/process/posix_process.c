#include "posix_process.h"

static void store_u64(u8 *destination, u64 value) {
    for (u32 index = 0; index < 8; index++)
        destination[index] = (u8)(value >> (8 * index));
}

static const char *arena_end(const struct posix_exec_vectors *vectors) {
    return vectors->arena + vectors->arena_bytes;
}

static int string_in_arena(const char *string,
                           const struct posix_exec_vectors *vectors) {
    return string && string >= vectors->arena && string < arena_end(vectors);
}

static u32 string_terminated(const char *string,
                             const struct posix_exec_vectors *vectors) {
    usize_t limit = (usize_t)(arena_end(vectors) - string);
    for (usize_t index = 0; index < limit; index++)
        if (!string[index]) return 1;
    return 0;
}

static int vectors_valid(const struct posix_exec_vectors *vectors) {
    if (!vectors || !vectors->arena) return POSIX_PROCESS_EINVAL;
    if (vectors->argc > POSIX_ARG_COUNT_MAX ||
        vectors->envc > POSIX_ARG_COUNT_MAX ||
        vectors->arena_bytes > POSIX_ARG_BYTES_MAX)
        return POSIX_PROCESS_E2BIG;
    for (u32 index = 0; index < vectors->argc; index++)
        if (!string_in_arena(vectors->argv[index], vectors) ||
            !string_terminated(vectors->argv[index], vectors))
            return POSIX_PROCESS_EINVAL;
    for (u32 index = 0; index < vectors->envc; index++)
        if (!string_in_arena(vectors->envp[index], vectors) ||
            !string_terminated(vectors->envp[index], vectors))
            return POSIX_PROCESS_EINVAL;
    return 0;
}

int posix_process_build_stack(u8 *stack_page,
                              const struct posix_exec_vectors *vectors,
                              u64 stack_base, u64 *stack_rsp) {
    int valid = vectors_valid(vectors);
    if (valid) return valid;
    if (!stack_page || !stack_rsp) return POSIX_PROCESS_EINVAL;
    u64 pointer_words =
        1 + (u64)vectors->argc + 1 + (u64)vectors->envc + 1;
    u64 total = pointer_words * 8 + vectors->arena_bytes;
    if (total > POSIX_STACK_PAGE) return POSIX_PROCESS_E2BIG;
    /* Padding keeps rsp 16-byte aligned; slack lands above the strings. */
    u32 rsp_offset = (u32)((POSIX_STACK_PAGE - total) & ~0xFu);

    /* The scratch page is kernel memory reused across execve calls, so it
       must not leak prior images' stack bytes into a new task. */
    for (u32 index = 0; index < POSIX_STACK_PAGE; index++)
        stack_page[index] = 0;

    u32 cursor = rsp_offset;
    store_u64(stack_page + cursor, vectors->argc);
    cursor += 8;
    u64 strings_base = stack_base + rsp_offset + pointer_words * 8;
    for (u32 index = 0; index < vectors->argc; index++) {
        u64 offset = (u64)(vectors->argv[index] - vectors->arena);
        store_u64(stack_page + cursor, strings_base + offset);
        cursor += 8;
    }
    store_u64(stack_page + cursor, 0);
    cursor += 8;
    for (u32 index = 0; index < vectors->envc; index++) {
        u64 offset = (u64)(vectors->envp[index] - vectors->arena);
        store_u64(stack_page + cursor, strings_base + offset);
        cursor += 8;
    }
    store_u64(stack_page + cursor, 0);
    cursor += 8;
    for (u32 index = 0; index < vectors->arena_bytes; index++)
        stack_page[cursor + index] = (u8)vectors->arena[index];

    *stack_rsp = stack_base + rsp_offset;
    return 0;
}
