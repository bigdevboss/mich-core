#include "types.h"
#include "posix_process.h"
#include "tests64.h"

#define STACK_BASE 0x1001FF000ULL

static u8 stack_page[POSIX_STACK_PAGE];
static char arena[POSIX_ARG_BYTES_MAX];
static char outside[8];

static u64 load_u64(const u8 *source) {
    u64 value = 0;
    for (u32 index = 0; index < 8; index++)
        value |= (u64)source[index] << (8 * index);
    return value;
}

static u32 page_offset(u64 address) {
    return (u32)(address - STACK_BASE);
}

static int page_string_equals(u32 offset, const char *expected) {
    u32 length = 0;
    while (expected[length]) {
        if (offset + length >= POSIX_STACK_PAGE ||
            stack_page[offset + length] != (u8)expected[length])
            return 0;
        length++;
    }
    return offset + length < POSIX_STACK_PAGE && !stack_page[offset + length];
}

static void fill_vectors(struct posix_exec_vectors *vectors, u32 argc,
                         u32 envc) {
    for (u32 index = 0; index < POSIX_ARG_COUNT_MAX; index++) {
        vectors->argv[index] = arena;
        vectors->envp[index] = arena;
    }
    vectors->argc = argc;
    vectors->envc = envc;
    vectors->arena_bytes = 0;
    vectors->arena = arena;
}

static u32 append_string(char *destination, const char *source) {
    u32 length = 0;
    while (source[length]) {
        destination[length] = source[length];
        length++;
    }
    destination[length] = 0;
    return length + 1;
}

int test_posix_process64(void) {
    struct posix_exec_vectors vectors;
    u64 rsp = 0;

    fill_vectors(&vectors, 2, 1);
    vectors.arena_bytes = append_string(arena, "one");
    vectors.argv[0] = arena;
    vectors.arena_bytes += append_string(arena + vectors.arena_bytes, "two");
    vectors.argv[1] = arena + 4;
    vectors.arena_bytes += append_string(arena + vectors.arena_bytes, "A=b");
    vectors.envp[0] = arena + 8;

    int valid = !posix_process_build_stack(stack_page, &vectors, STACK_BASE,
                                           &rsp);
    valid = valid && rsp >= STACK_BASE && rsp < STACK_BASE + POSIX_STACK_PAGE &&
        !(rsp & 0xF);
    u32 offset = page_offset(rsp);
    valid = valid && load_u64(stack_page + offset) == 2;
    u64 argv0 = load_u64(stack_page + offset + 8);
    u64 argv1 = load_u64(stack_page + offset + 16);
    u64 argv_null = load_u64(stack_page + offset + 24);
    u64 envp0 = load_u64(stack_page + offset + 32);
    u64 envp_null = load_u64(stack_page + offset + 40);
    valid = valid && page_string_equals(page_offset(argv0), "one") &&
        page_string_equals(page_offset(argv1), "two") &&
        page_string_equals(page_offset(envp0), "A=b") &&
        !argv_null && !envp_null;

    fill_vectors(&vectors, 0, 0);
    rsp = 0;
    valid = valid &&
        !posix_process_build_stack(stack_page, &vectors, STACK_BASE, &rsp) &&
        !(rsp & 0xF) && load_u64(stack_page + page_offset(rsp)) == 0 &&
        !load_u64(stack_page + page_offset(rsp) + 8) &&
        !load_u64(stack_page + page_offset(rsp) + 16);

    fill_vectors(&vectors, POSIX_ARG_COUNT_MAX + 1, 0);
    valid = valid && posix_process_build_stack(stack_page, &vectors,
                                               STACK_BASE, &rsp) ==
        POSIX_PROCESS_E2BIG;
    fill_vectors(&vectors, 0, POSIX_ARG_COUNT_MAX + 1);
    valid = valid && posix_process_build_stack(stack_page, &vectors,
                                               STACK_BASE, &rsp) ==
        POSIX_PROCESS_E2BIG;

    fill_vectors(&vectors, 1, 0);
    vectors.arena_bytes = POSIX_ARG_BYTES_MAX + 1;
    valid = valid && posix_process_build_stack(stack_page, &vectors,
                                               STACK_BASE, &rsp) ==
        POSIX_PROCESS_E2BIG;

    fill_vectors(&vectors, 1, 0);
    arena[0] = 'x';
    vectors.arena_bytes = 1;
    valid = valid && posix_process_build_stack(stack_page, &vectors,
                                               STACK_BASE, &rsp) ==
        POSIX_PROCESS_EINVAL;

    fill_vectors(&vectors, 1, 0);
    vectors.arena_bytes = append_string(arena, "one");
    vectors.argv[0] = outside;
    valid = valid && posix_process_build_stack(stack_page, &vectors,
                                               STACK_BASE, &rsp) ==
        POSIX_PROCESS_EINVAL;

    valid = valid &&
        posix_process_build_stack(stack_page, 0, STACK_BASE, &rsp) ==
        POSIX_PROCESS_EINVAL &&
        posix_process_build_stack(0, &vectors, STACK_BASE, &rsp) ==
        POSIX_PROCESS_EINVAL &&
        posix_process_build_stack(stack_page, &vectors, STACK_BASE, 0) ==
        POSIX_PROCESS_EINVAL;
    fill_vectors(&vectors, 1, 0);
    vectors.arena = 0;
    valid = valid && posix_process_build_stack(stack_page, &vectors,
                                               STACK_BASE, &rsp) ==
        POSIX_PROCESS_EINVAL;

    return valid ? 0 : -1;
}
