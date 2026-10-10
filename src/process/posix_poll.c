#include "posix_poll.h"
#include "posix_fd.h"
#include "posix_signal.h"
#include "posix_vfs.h"
#include "rtc64.h"
#include "runtime64.h"
#include "scheduler.h"
#include "task.h"
#include "klock.h"
#include "vm64.h"

// One timer tick in the milliseconds poll speaks.
#define POLL_TICK_MS (1000u / RTC64_TICK_HZ)

// The scheduler's tick counter, the clock a poll deadline is measured on.
extern u32 timer_ticks;

struct posix_poll_wait {
    uptr_t request;
    u32 count;
    struct posix_poll_fd fds[POSIX_POLL_FD_MAX];
};

static struct posix_poll_wait waits[MAX_TASKS];
// The park and the wake that answers it have to be ordered against each
// other: a descriptor turning ready between the caller's scan and the state
// change wakes nobody, and the poll then never returns.
static struct klock posix_poll_wait_klock = KLOCK_INIT(KLOCK_LEVEL_WAIT);

// Syscalls run with interrupts masked on this single CPU, so poll wait
// state mutates from one context at a time: the syscall that parks, the
// wake path that completes, or the timer tick that closes a deadline.
// The wake paths of every waitable object run on that same serialization.

static u32 poll_scan(struct task *task, const struct posix_poll_fd *fds,
                     u32 count, u16 *revents) {
    u32 ready = 0;
    for (u32 index = 0; index < count; index++) {
        u16 hit = posix_fd_poll_events(task, fds[index].descriptor);
        // The caller reads back only the events it asked for; the three
        // error bits are the answers it cannot ask for and always land.
        hit &= fds[index].events |
            (POSIX_POLLERR | POSIX_POLLHUP | POSIX_POLLNVAL);
        revents[index] = hit;
        if (hit) ready++;
    }
    return ready;
}

static int poll_write_back(struct task *task, uptr_t request,
                           const struct posix_poll_wait *wait,
                           const u16 *revents) {
    struct posix_poll_request out;
    out.count = wait->count;
    out.reserved = 0;
    for (u32 index = 0; index < POSIX_POLL_FD_MAX; index++)
        out.fds[index] = wait->fds[index];
    for (u32 index = 0; index < wait->count; index++)
        out.fds[index].revents = revents[index];
    return vm64_copy_to(task->page_dir, request, &out, sizeof(out));
}

// Complete a parked poll from its own record: a descriptor came ready, or
// the deadline closed. Anything else leaves the caller parked until the
// next change on one of the objects it listed.
static void poll_complete(u32 slot, int deadline) {
    struct task *task = &task_pool[slot];
    u16 revents[POSIX_POLL_FD_MAX];
    klock_acquire(&posix_poll_wait_klock);
    u32 ready = poll_scan(task, waits[slot].fds, waits[slot].count, revents);
    if (!ready && !deadline) {
        klock_release(&posix_poll_wait_klock);
        return;
    }
    i64 answer = (i64)ready;
    if (poll_write_back(task, waits[slot].request, &waits[slot], revents))
        answer = POSIX_VFS_EIO;
    waits[slot].request = 0;
    waits[slot].count = 0;
    task->poll_deadline = 0;
    task_state_set(task, TASK_RUNNING);
    task64_set_result(slot, answer);
    klock_release(&posix_poll_wait_klock);
}

static i64 poll_answer_ready(struct task *task, uptr_t request,
                             const struct posix_poll_request *in,
                             u16 *revents, u32 ready) {
    struct posix_poll_wait snapshot;
    for (u32 index = 0; index < POSIX_POLL_FD_MAX; index++)
        snapshot.fds[index] = in->fds[index];
    snapshot.request = request;
    snapshot.count = in->count;
    if (poll_write_back(task, request, &snapshot, revents))
        return POSIX_VFS_EIO;
    return (i64)ready;
}

i64 posix_poll(struct task *task, uptr_t request,
               const struct posix_poll_request *in, i64 timeout_ms) {
    int slot = scheduler_current();
    if (slot < 0 || &task_pool[slot] != task) return POSIX_VFS_EIO;
    if (!in || !in->count || in->count > POSIX_POLL_FD_MAX || timeout_ms < -1)
        return POSIX_VFS_EINVAL;
    u16 revents[POSIX_POLL_FD_MAX];
    u32 ready = poll_scan(task, in->fds, in->count, revents);
    if (!ready && timeout_ms) {
        klock_acquire(&posix_poll_wait_klock);
        ready = poll_scan(task, in->fds, in->count, revents);
        if (!ready) {
            waits[slot].request = request;
            waits[slot].count = in->count;
            for (u32 index = 0; index < in->count; index++)
                waits[slot].fds[index] = in->fds[index];
            // The deadline rounds up by construction, so the park never
            // wakes early on a partial tick, the way nanosleep reads its
            // interval.
            task->poll_deadline = 0;
            if (timeout_ms > 0) {
                u64 ticks = ((u64)timeout_ms + POLL_TICK_MS - 1u) /
                    POLL_TICK_MS;
                if (ticks > 0xFFFFFFFEull) ticks = 0xFFFFFFFEull;
                task->poll_deadline = timer_ticks + (u32)ticks;
            }
            task_state_set(task, TASK_BLOCKED_POLL);
        }
        klock_release(&posix_poll_wait_klock);
        if (ready) return poll_answer_ready(task, request, in, revents, ready);
        if (scheduler_pick_next(slot) < 0) {
            // Nothing else can run, so the park would freeze the CPU
            // inside the syscall. The pipe and ipc parks answer the same
            // deadlock.
            waits[slot].request = 0;
            waits[slot].count = 0;
            task->poll_deadline = 0;
            task_state_set(task, TASK_RUNNING);
            return POSIX_VFS_EDEADLK;
        }
        return (int)task64_block_switch();
    }
    return poll_answer_ready(task, request, in, revents, ready);
}

i64 posix_poll_signal(struct task *target) {
    u32 slot = (u32)(target - task_pool);
    if (slot >= (u32)MAX_TASKS || target->state != TASK_BLOCKED_POLL) return 0;
    klock_acquire(&posix_poll_wait_klock);
    waits[slot].request = 0;
    waits[slot].count = 0;
    target->poll_deadline = 0;
    klock_release(&posix_poll_wait_klock);
    return POSIX_SIGNAL_EINTR;
}

void posix_poll_notify(void) {
    for (u32 slot = 0; slot < (u32)task_pool_count; slot++)
        if (task_pool[slot].state == TASK_BLOCKED_POLL) poll_complete(slot, 0);
}

void posix_poll_tick(u32 now) {
    for (u32 slot = 0; slot < (u32)task_pool_count; slot++) {
        struct task *task = &task_pool[slot];
        if (task->state != TASK_BLOCKED_POLL || !task->poll_deadline) continue;
        if ((i32)(now - task->poll_deadline) < 0) continue;
        poll_complete(slot, 1);
    }
}
