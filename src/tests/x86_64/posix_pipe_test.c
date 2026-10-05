#include "types.h"
#include "posix_abi.h"
#include "posix_pipe.h"
#include "posix_pledge.h"
#include "posix_profile.h"
#include "task.h"
#include "tests64.h"

// The ring and park paths need a live scheduler and user memory, which the
// prod demo exercises; the unit scope is what the pool alone can prove:
// exhaustion, end counting, recycling, and the promise gate row.

int test_posix_pipe64(void) {
    int valid = 1;

    // The pool starts empty for this runner and every pipe the test takes
    // goes back, so the exhaustion count is exact.
    valid = valid && posix_pipe_active_count() == 0;

    // Every pool slot hands out exactly once; the next ask answers ENFILE.
    int indices[POSIX_PIPE_MAX];
    for (u32 index = 0; index < POSIX_PIPE_MAX; index++)
        indices[index] = posix_pipe_create();
    for (u32 index = 0; index < POSIX_PIPE_MAX; index++)
        valid = valid && indices[index] == (int)index;
    valid = valid && posix_pipe_create() == POSIX_VFS_ENFILE;
    valid = valid && posix_pipe_active_count() == POSIX_PIPE_MAX;

    // A pipe lives while either end count is above zero: a dup holds the
    // end open, and the pool slot frees only when both counts fall back.
    posix_pipe_retain(0, POSIX_PIPE_END_READ);
    posix_pipe_retain(0, POSIX_PIPE_END_READ);
    posix_pipe_retain(0, POSIX_PIPE_END_WRITE);
    posix_pipe_release(0, POSIX_PIPE_END_READ);
    valid = valid && posix_pipe_active_count() == POSIX_PIPE_MAX;
    posix_pipe_release(0, POSIX_PIPE_END_READ);
    posix_pipe_release(0, POSIX_PIPE_END_WRITE);
    valid = valid && posix_pipe_active_count() == POSIX_PIPE_MAX - 1u;
    valid = valid && posix_pipe_create() == 0;
    valid = valid && posix_pipe_active_count() == POSIX_PIPE_MAX;

    // Out of range indexes and unknown ends stay inert rather than
    // corrupting the pool.
    posix_pipe_retain(POSIX_PIPE_MAX, POSIX_PIPE_END_READ);
    posix_pipe_release(POSIX_PIPE_MAX, POSIX_PIPE_END_READ);
    posix_pipe_retain(0, 7u);
    posix_pipe_release(0, 7u);
    valid = valid && posix_pipe_active_count() == POSIX_PIPE_MAX;

    for (u32 index = 0; index < POSIX_PIPE_MAX; index++) {
        posix_pipe_retain(index, POSIX_PIPE_END_READ);
        posix_pipe_retain(index, POSIX_PIPE_END_WRITE);
    }
    for (u32 index = 0; index < POSIX_PIPE_MAX; index++) {
        posix_pipe_release(index, POSIX_PIPE_END_READ);
        posix_pipe_release(index, POSIX_PIPE_END_WRITE);
    }
    valid = valid && posix_pipe_active_count() == 0;

    // Releasing an end nobody retained is a no-op, so a fresh pipe does
    // not vanish under a stray close.
    valid = valid && posix_pipe_create() == 0;
    posix_pipe_release(0, POSIX_PIPE_END_WRITE);
    valid = valid && posix_pipe_active_count() == 1;
    posix_pipe_retain(0, POSIX_PIPE_END_READ);
    posix_pipe_retain(0, POSIX_PIPE_END_WRITE);
    posix_pipe_release(0, POSIX_PIPE_END_READ);
    posix_pipe_release(0, POSIX_PIPE_END_WRITE);
    valid = valid && posix_pipe_active_count() == 0;

    // The promise gate row: pipe rides with stdio, the way OpenBSD
    // carries it, and a narrowed set denies it loudly or quietly.
    struct task *probe = task_alloc_slot();
    if (probe && !posix_profile_admit(probe)) {
        valid = valid && posix_pledge_promise(probe, "stdio error", 0) == 0;
        valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_PIPE) == 0;
        valid = valid && posix_pledge_promise(probe, "error", 0) == 0;
        valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_PIPE) ==
                        POSIX_PLEDGE_ENOSYS;
        valid = valid && posix_pledge_promise(probe, "", 0) == 0;
        valid = valid && posix_pledge_gate(probe, POSIX_SYSCALL_PIPE) == 1;
        posix_pledge_reset(probe);
        posix_profile_release(probe);
        task_free_slot(probe);
    } else {
        if (probe) task_free_slot(probe);
        valid = 0;
    }

    return valid ? 0 : -1;
}
