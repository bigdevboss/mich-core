#include "posix_pipe.h"
#include "posix_abi.h"
#include "posix_signal.h"
#include "posix_vfs.h"
#include "task.h"
#include "vm64.h"
#include "scheduler.h"
#include "runtime64.h"

// The parked dispatch frame is abandoned on the switch, so the peer that
// completes the operation patches the parked caller's request in place:
// the data array and the transferred word at these fixed offsets.
#define IO_TRANSFERRED_OFFSET 8u
#define IO_DATA_OFFSET 12u

struct posix_pipe_state {
    u8 ring[POSIX_PIPE_BUF];
    u32 tail;
    u32 count;
    u32 readers;
    u32 writers;
    u32 active;
};

struct pipe_wait {
    u32 pipe;
    u32 end;
    uptr_t request;
    u32 length;
};

static struct posix_pipe_state pipes[POSIX_PIPE_MAX];
static struct pipe_wait waits[MAX_TASKS];

// Syscalls run with interrupts masked on this single CPU, so pipe state
// mutates only from syscall context, one task at a time; the wake paths
// run in the caller's own syscall. The ipc layer relies on the same
// serialization, and the fd layer's spinlock never crosses into here.
// Per-pipe locks arrive with the second CPU, not before.

static u32 ring_free(const struct posix_pipe_state *pipe) {
    return POSIX_PIPE_BUF - pipe->count;
}

static void ring_put(struct posix_pipe_state *pipe, const u8 *source,
                     u32 length) {
    u32 head = (pipe->tail + pipe->count) % POSIX_PIPE_BUF;
    for (u32 index = 0; index < length; index++) {
        pipe->ring[head] = source[index];
        head = (head + 1u) % POSIX_PIPE_BUF;
    }
    pipe->count += length;
}

static void ring_take(struct posix_pipe_state *pipe, u8 *destination,
                      u32 length) {
    for (u32 index = 0; index < length; index++) {
        destination[index] = pipe->ring[pipe->tail];
        pipe->tail = (pipe->tail + 1u) % POSIX_PIPE_BUF;
    }
    pipe->count -= length;
}

static int patch_result(struct task *task, uptr_t request, u32 transferred) {
    return vm64_copy_to(task->page_dir, request + IO_TRANSFERRED_OFFSET,
                        &transferred, sizeof(transferred));
}

// Hand the ring's bytes to the parked readers of one pipe. Each reader
// takes what is there up to what it asked for; the loop stops when the
// ring drains. Readers parked on an empty ring stay parked, which is
// where the EOF path below picks them up.
static void wake_readers(u32 index) {
    struct posix_pipe_state *pipe = &pipes[index];
    u8 staging[POSIX_IO_MAX];
    for (u32 slot = 0; slot < (u32)task_pool_count; slot++) {
        if (task_pool[slot].state != TASK_BLOCKED_PIPE ||
            waits[slot].pipe != index + 1u ||
            waits[slot].end != POSIX_PIPE_END_READ)
            continue;
        if (!pipe->count) return;
        struct task *reader = &task_pool[slot];
        u32 take = pipe->count < waits[slot].length ? pipe->count :
            waits[slot].length;
        ring_take(pipe, staging, take);
        i64 answer = (i64)take;
        u32 transferred = take;
        if (vm64_copy_to(reader->page_dir, waits[slot].request +
                         IO_DATA_OFFSET, staging, take) ||
            patch_result(reader, waits[slot].request, transferred))
            answer = (i64)POSIX_VFS_EIO;
        waits[slot].pipe = 0;
        waits[slot].request = 0;
        waits[slot].length = 0;
        reader->state = TASK_RUNNING;
        task64_set_result(slot, answer);
    }
}

// Complete the parked writers of one pipe. A writer parks only when its
// whole request would not fit, and a request never exceeds PIPE_BUF, so
// each completion copies the writer's full request into the freed space.
static void wake_writers(u32 index) {
    struct posix_pipe_state *pipe = &pipes[index];
    u8 staging[POSIX_IO_MAX];
    for (u32 slot = 0; slot < (u32)task_pool_count; slot++) {
        if (task_pool[slot].state != TASK_BLOCKED_PIPE ||
            waits[slot].pipe != index + 1u ||
            waits[slot].end != POSIX_PIPE_END_WRITE)
            continue;
        if (waits[slot].length > ring_free(pipe)) return;
        struct task *writer = &task_pool[slot];
        if (vm64_copy_from(writer->page_dir, staging, waits[slot].request +
                           IO_DATA_OFFSET, waits[slot].length)) {
            waits[slot].pipe = 0;
            waits[slot].request = 0;
            waits[slot].length = 0;
            writer->state = TASK_RUNNING;
            task64_set_result(slot, (i64)POSIX_VFS_EIO);
            continue;
        }
        ring_put(pipe, staging, waits[slot].length);
        u32 transferred = waits[slot].length;
        patch_result(writer, waits[slot].request, transferred);
        waits[slot].pipe = 0;
        waits[slot].request = 0;
        waits[slot].length = 0;
        writer->state = TASK_RUNNING;
        task64_set_result(slot, (i64)transferred);
    }
}

// The last read end closed: every parked writer answers EPIPE. The
// dispatch layer converts that answer into the SIGPIPE the writer owes,
// the same dance an immediate write past a closed read end takes.
static void wake_writers_broken(u32 index) {
    for (u32 slot = 0; slot < (u32)task_pool_count; slot++) {
        if (task_pool[slot].state != TASK_BLOCKED_PIPE ||
            waits[slot].pipe != index + 1u ||
            waits[slot].end != POSIX_PIPE_END_WRITE)
            continue;
        struct task *writer = &task_pool[slot];
        patch_result(writer, waits[slot].request, 0);
        waits[slot].pipe = 0;
        waits[slot].request = 0;
        waits[slot].length = 0;
        writer->state = TASK_RUNNING;
        task64_set_result(slot, (i64)POSIX_VFS_EPIPE);
    }
}

int posix_pipe_create(void) {
    for (u32 index = 0; index < POSIX_PIPE_MAX; index++) {
        if (pipes[index].active) continue;
        pipes[index].tail = 0;
        pipes[index].count = 0;
        pipes[index].readers = 0;
        pipes[index].writers = 0;
        pipes[index].active = 1;
        return (int)index;
    }
    return POSIX_VFS_ENFILE;
}

void posix_pipe_retain(u32 index, u32 end) {
    if (index >= POSIX_PIPE_MAX || !pipes[index].active) return;
    if (end == POSIX_PIPE_END_READ) pipes[index].readers++;
    else if (end == POSIX_PIPE_END_WRITE) pipes[index].writers++;
}

void posix_pipe_release(u32 index, u32 end) {
    if (index >= POSIX_PIPE_MAX || !pipes[index].active) return;
    if (end == POSIX_PIPE_END_READ) {
        if (!pipes[index].readers) return;
        pipes[index].readers--;
        if (pipes[index].readers) return;
        wake_writers_broken(index);
    } else if (end == POSIX_PIPE_END_WRITE) {
        if (!pipes[index].writers) return;
        pipes[index].writers--;
        if (pipes[index].writers) return;
        // Data still in the ring reads out first; the readers parked on
        // the emptied ring answer EOF.
        wake_readers(index);
        for (u32 slot = 0; slot < (u32)task_pool_count; slot++) {
            if (task_pool[slot].state != TASK_BLOCKED_PIPE ||
                waits[slot].pipe != index + 1u ||
                waits[slot].end != POSIX_PIPE_END_READ)
                continue;
            patch_result(&task_pool[slot], waits[slot].request, 0);
            waits[slot].pipe = 0;
            waits[slot].request = 0;
            waits[slot].length = 0;
            task_pool[slot].state = TASK_RUNNING;
            task64_set_result(slot, 0);
        }
    } else {
        // An end the pool never handed out holds nothing to release.
        return;
    }
    if (!pipes[index].readers && !pipes[index].writers)
        pipes[index].active = 0;
}

int posix_pipe_io(struct task *task, u32 index, u32 end, uptr_t request,
                  u32 length) {
    if (index >= POSIX_PIPE_MAX || !pipes[index].active ||
        !length || length > POSIX_IO_MAX)
        return POSIX_VFS_EINVAL;
    struct posix_pipe_state *pipe = &pipes[index];
    int slot = scheduler_current();
    if (slot < 0 || &task_pool[slot] != task) return POSIX_VFS_EIO;
    u8 staging[POSIX_IO_MAX];

    if (end == POSIX_PIPE_END_READ) {
        if (pipe->count) {
            u32 take = pipe->count < length ? pipe->count : length;
            ring_take(pipe, staging, take);
            u32 transferred = take;
            if (vm64_copy_to(task->page_dir, request + IO_DATA_OFFSET,
                             staging, take) ||
                patch_result(task, request, transferred))
                return POSIX_VFS_EIO;
            // The space opens before the writers wake, so each completed
            // writer finds room for its whole request.
            wake_writers(index);
            return (int)take;
        }
        if (!pipe->writers) {
            patch_result(task, request, 0);
            return 0;
        }
        waits[slot].pipe = index + 1u;
        waits[slot].end = POSIX_PIPE_END_READ;
        waits[slot].request = request;
        waits[slot].length = length;
        task->state = TASK_BLOCKED_PIPE;
        if (scheduler_pick_next(slot) < 0) {
            // Nothing else can run, so the park would freeze the CPU
            // inside the syscall. The ipc sender answers EDEADLK for
            // the same reason.
            waits[slot].pipe = 0;
            waits[slot].request = 0;
            waits[slot].length = 0;
            task->state = TASK_RUNNING;
            return POSIX_VFS_EDEADLK;
        }
        return (int)task64_block_switch();
    }

    if (end != POSIX_PIPE_END_WRITE) return POSIX_VFS_EINVAL;
    if (!pipe->readers) return POSIX_VFS_EPIPE;
    // A write of at most PIPE_BUF is atomic: it lands whole or parks.
    // The io request bound already keeps every write under PIPE_BUF.
    if (length > ring_free(pipe)) {
        waits[slot].pipe = index + 1u;
        waits[slot].end = POSIX_PIPE_END_WRITE;
        waits[slot].request = request;
        waits[slot].length = length;
        task->state = TASK_BLOCKED_PIPE;
        if (scheduler_pick_next(slot) < 0) {
            waits[slot].pipe = 0;
            waits[slot].request = 0;
            waits[slot].length = 0;
            task->state = TASK_RUNNING;
            return POSIX_VFS_EDEADLK;
        }
        return (int)task64_block_switch();
    }
    if (vm64_copy_from(task->page_dir, staging, request + IO_DATA_OFFSET,
                       length))
        return POSIX_VFS_EIO;
    ring_put(pipe, staging, length);
    u32 transferred = length;
    patch_result(task, request, transferred);
    wake_readers(index);
    return (int)length;
}

i64 posix_pipe_signal(struct task *target) {
    u32 slot = (u32)(target - task_pool);
    if (slot >= (u32)MAX_TASKS || target->state != TASK_BLOCKED_PIPE)
        return 0;
    waits[slot].pipe = 0;
    waits[slot].request = 0;
    waits[slot].length = 0;
    return POSIX_SIGNAL_EINTR;
}

u32 posix_pipe_active_count(void) {
    u32 active = 0;
    for (u32 index = 0; index < POSIX_PIPE_MAX; index++)
        if (pipes[index].active) active++;
    return active;
}
