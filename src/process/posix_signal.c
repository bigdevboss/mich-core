#include "posix_signal.h"
#include "posix_pipe.h"
#include "posix_socket.h"
#include "posix_poll.h"
#include "posix_abi.h"
#include "runtime64.h"
#include "task.h"
#include "vm64.h"

extern u32 timer_ticks;

// Per-slot signal state rather than fields in struct task: the table stays
// bounded by MAX_TASKS and the task struct carries only what the scheduler
// and the IPC paths read on their own. A zeroed entry is the default
// disposition with an empty mask, which task reset restores.
struct posix_signal_state {
    u64 pending;
    u64 blocked;
    uptr_t handler[POSIX_SIG_COUNT];
    uptr_t restorer[POSIX_SIG_COUNT];
    u64 handler_mask[POSIX_SIG_COUNT];
};

static struct posix_signal_state states[MAX_TASKS];

static struct posix_signal_state *state_for(struct task *task) {
    u32 slot = (u32)(task - task_pool);
    if (slot >= MAX_TASKS) return 0;
    return &states[slot];
}

// The bounded set: everything else is EINVAL at the syscall boundary, so
// no signal number outside the list ever reaches the tables.
static int sig_known(u32 signo) {
    switch (signo) {
    case POSIX_SIG_HUP:
    case POSIX_SIG_INT:
    case POSIX_SIG_QUIT:
    case POSIX_SIG_ILL:
    case POSIX_SIG_ABRT:
    case POSIX_SIG_FPE:
    case POSIX_SIG_KILL:
    case POSIX_SIG_USR1:
    case POSIX_SIG_SEGV:
    case POSIX_SIG_USR2:
    case POSIX_SIG_PIPE:
    case POSIX_SIG_ALRM:
    case POSIX_SIG_TERM:
    case POSIX_SIG_CHLD:
        return 1;
    }
    return 0;
}

// Signals whose default action terminates the process. SIGCHLD defaults to
// ignore and SIGKILL is handled before dispositions are ever consulted.
static int sig_default_term(u32 signo) {
    return signo != POSIX_SIG_CHLD;
}

void posix_signal_reset(struct task *task) {
    struct posix_signal_state *state = state_for(task);
    if (!state) return;
    u8 *bytes = (u8 *)state;
    for (u32 index = 0; index < sizeof(*state); index++) bytes[index] = 0;
}

void posix_signal_fork(struct task *parent, struct task *child) {
    struct posix_signal_state *from = state_for(parent);
    struct posix_signal_state *to = state_for(child);
    if (!from || !to) return;
    u8 *dst = (u8 *)to;
    const u8 *src = (const u8 *)from;
    for (u32 index = 0; index < sizeof(*to); index++) dst[index] = src[index];
    // POSIX leaves the child's pending set empty: a signal aimed at the
    // parent must not fire a handler twice.
    to->pending = 0;
}

void posix_signal_exec(struct task *task) {
    struct posix_signal_state *state = state_for(task);
    if (!state) return;
    // A caught handler is a user address that dies with the old image, so
    // exec folds it back to the default. An explicit ignore is image
    // independent and survives, as does the blocked mask.
    for (u32 signo = 1u; signo <= POSIX_SIG_COUNT; signo++) {
        if (state->handler[signo - 1u] > POSIX_SIG_IGN)
            state->handler[signo - 1u] = POSIX_SIG_DFL;
    }
}

u32 posix_signal_fault_signo(u32 vector) {
    switch (vector) {
    case 0u:
        return POSIX_SIG_FPE;
    case 6u:
        return POSIX_SIG_ILL;
    case 13u:
    case 14u:
        return POSIX_SIG_SEGV;
    }
    return 0u;
}

// Break a park the way the owning dispatcher would: put the task back to
// RUNNING and publish the result its syscall frame will return. The sleep
// park additionally answers the interrupted nanosleep with the time it
// still owed, which POSIX reports through the remainder pointer.
static void wake_with_eintr(struct task *target) {
    u32 slot = (u32)(target - task_pool);
    if (target->state == TASK_BLOCKED_SLEEP) {
        u32 deadline = target->sleep_deadline;
        uptr_t request = target->sleep_request;
        target->sleep_deadline = 0;
        target->sleep_request = 0;
        target->state = TASK_RUNNING;
        task64_set_result(slot, POSIX_SIGNAL_EINTR);
        if (request) {
            u32 left = deadline - timer_ticks;
            if ((i32)left <= 0) left = 0;
            u64 nsec = (u64)left * (1000000000u / 100u);
            struct { u64 sec; u64 nsec; } remaining;
            remaining.sec = nsec / 1000000000u;
            remaining.nsec = nsec % 1000000000u;
            vm64_copy_to(target->page_dir, request + 16, &remaining,
                         sizeof(remaining));
        }
        return;
    }
    if (target->state == TASK_BLOCKED_PIPE) {
        i64 answer = posix_pipe_signal(target);
        target->state = TASK_RUNNING;
        task64_set_result(slot, answer);
        return;
    }
    if (target->state == TASK_BLOCKED_SOCKET) {
        i64 answer = posix_socket_signal(target);
        target->state = TASK_RUNNING;
        task64_set_result(slot, answer);
        return;
    }
    if (target->state == TASK_BLOCKED_POLL) {
        i64 answer = posix_poll_signal(target);
        target->state = TASK_RUNNING;
        task64_set_result(slot, answer);
        return;
    }
    if (target->state == TASK_BLOCKED_WAIT) {
        target->wait_pid = -1;
        target->wait_posix = 0;
        target->wait_status_address = 0;
        target->state = TASK_RUNNING;
        task64_set_result(slot, POSIX_SIGNAL_EINTR);
    }
}

int posix_signal_kill(struct task *caller, int pid, u32 signo) {
    // The profile has no process groups, so a non positive pid has no
    // meaning to hide behind: reject it rather than guess a group.
    if (pid <= 0) return POSIX_SIGNAL_EINVAL;
    if (signo > 31u || (signo && !sig_known(signo)))
        return POSIX_SIGNAL_EINVAL;
    u32 slot = PID_SLOT((u32)pid);
    if (!slot || slot >= (u32)task_pool_count ||
        task_pool[slot].id != pid ||
        task_pool[slot].state == TASK_FREE ||
        task_pool[slot].state == TASK_ZOMBIE)
        return POSIX_SIGNAL_ESRCH;
    struct task *target = &task_pool[slot];
    if (caller->uid != target->uid && caller->uid != 0)
        return POSIX_SIGNAL_EPERM;
    if (!signo) return 0;
    if (signo == POSIX_SIG_KILL) return 1;
    struct posix_signal_state *state = state_for(target);
    if (!state) return POSIX_SIGNAL_ESRCH;
    u64 bit = POSIX_SIGNAL_BIT(signo);
    if (state->handler[signo - 1u] == POSIX_SIG_IGN) return 0;
    if (state->blocked & bit) {
        // Blocked stays pending and wakes nothing: POSIX holds the signal
        // until the mask opens, even when its action would kill.
        state->pending |= bit;
        return 0;
    }
    if (state->handler[signo - 1u] == POSIX_SIG_DFL) {
        if (!sig_default_term(signo)) return 0;
        return 1;
    }
    state->pending |= bit;
    wake_with_eintr(target);
    return 0;
}

int posix_signal_action(struct task *task, u32 signo, void *record) {
    struct posix_sigaction_request *request = record;
    struct posix_signal_state *state = state_for(task);
    if (!state || !signo || signo > 31u || signo == POSIX_SIG_KILL)
        return POSIX_SIGNAL_EINVAL;
    // SIGKILL rejects the call outright in the dispatcher; nothing else in
    // the bounded set is untouchable, so the swap is unconditional.
    u32 index = signo - 1u;
    // The previous set is read before the swap, since the new values land
    // in the same slots the answer copies from. Without the apply flag the
    // call is a query: the dispositions stay exactly as they were.
    uptr_t previous_handler = state->handler[index];
    uptr_t previous_restorer = state->restorer[index];
    u64 previous_mask = state->handler_mask[index];
    if (request->flags & POSIX_SA_APPLY) {
        state->handler[index] = request->handler;
        state->restorer[index] =
            request->handler > POSIX_SIG_IGN ? request->restorer : 0;
        state->handler_mask[index] = request->mask;
    }
    request->previous_handler = previous_handler;
    request->previous_restorer = previous_restorer;
    request->previous_mask = previous_mask;
    request->previous_flags =
        previous_handler > POSIX_SIG_IGN ? POSIX_SA_RESTORER : 0u;
    return 0;
}

int posix_signal_procmask(struct task *task, u32 how, void *record) {
    struct posix_sigprocmask_request *request = record;
    struct posix_signal_state *state = state_for(task);
    if (!state) return POSIX_SIGNAL_ESRCH;
    u64 previous = state->blocked;
    if (how == POSIX_SIG_BLOCK) state->blocked |= request->mask;
    else if (how == POSIX_SIG_UNBLOCK) state->blocked &= ~request->mask;
    else if (how == POSIX_SIG_SETMASK) state->blocked = request->mask;
    else return POSIX_SIGNAL_EINVAL;
    // SIGKILL stays unblockable whatever the caller asked for, which is
    // the whole point of the signal.
    state->blocked &= ~POSIX_SIGNAL_BIT(POSIX_SIG_KILL);
    request->previous = previous;
    return 0;
}

void posix_signal_pending(struct task *task, u64 *pending) {
    struct posix_signal_state *state = state_for(task);
    if (!pending) return;
    *pending = state ? (state->pending & state->blocked) : 0;
}

u32 posix_signal_pick(struct task *task) {
    struct posix_signal_state *state = state_for(task);
    if (!state) return 0u;
    u64 deliverable = state->pending & ~state->blocked;
    if (!deliverable) return 0u;
    u32 signo = 1u;
    while (!(deliverable & 1u)) {
        deliverable >>= 1;
        signo++;
    }
    return signo;
}

int posix_signal_deliver(struct task *task, struct posix_signal_regs *regs,
                         u32 forced) {
    struct posix_signal_state *state = state_for(task);
    if (!state) return 0;
    u32 signo = 0;
    if (forced) {
        // A hardware fault reports through its own delivery: no kill
        // posted a bit, and a blocked mask cannot hold a fault back.
        signo = forced;
    } else {
        u64 deliverable = state->pending & ~state->blocked;
        if (!deliverable) return 0;
        signo = posix_signal_pick(task);
    }
    u64 bit = POSIX_SIGNAL_BIT(signo);
    uptr_t handler = state->handler[signo - 1u];
    // Re-read the disposition here: an exec or a sigaction between post
    // and delivery can have retired the handler, and a default that
    // terminates cannot ride a frame.
    if (handler <= POSIX_SIG_IGN) {
        if (!forced) state->pending &= ~bit;
        if (handler == POSIX_SIG_IGN || !sig_default_term(signo)) return 0;
        return -(int)signo;
    }
    struct posix_sigframe frame;
    frame.restorer = state->restorer[signo - 1u];
    frame.signo = signo;
    frame.saved_mask = state->blocked;
    frame.rax = regs->rax;
    frame.rcx = regs->rcx;
    frame.rdx = regs->rdx;
    frame.rsi = regs->rsi;
    frame.rdi = regs->rdi;
    frame.r8 = regs->r8;
    frame.r9 = regs->r9;
    frame.r10 = regs->r10;
    frame.r11 = regs->r11;
    frame.rip = regs->rip;
    frame.rsp = regs->rsp;
    frame.rflags = regs->rflags;
    // Land the frame so the handler enters with a return address on top
    // and an ABI straight stack: the low bits clear, minus the eight the
    // call convention leaves for the pushed return address.
    u64 top = ((regs->rsp - sizeof(frame)) & ~0xFull) - 8u;
    if (vm64_copy_to(task->page_dir, top, &frame, sizeof(frame)))
        return -(int)POSIX_SIG_SEGV;
    if (!forced) state->pending &= ~bit;
    state->blocked |= state->handler_mask[signo - 1u] | bit;
    regs->rip = handler;
    regs->rsp = top;
    regs->rdi = signo;
    return 1;
}

int posix_signal_restore(struct task *task, u32 space, uptr_t frame_address,
                         struct posix_signal_regs *regs) {
    struct posix_signal_state *state = state_for(task);
    struct posix_sigframe frame;
    if (!state || (frame_address >> 47) != 0 ||
        vm64_copy_from(task->page_dir, &frame, frame_address, sizeof(frame)))
        return POSIX_SIGNAL_EINVAL;
    // The iret return path has no validate_return after it, so the frame
    // is checked here against the same contract: canonical user addresses
    // with an executable rip and a writable stack.
    int rip_canonical = (frame.rip >> 47) == 0;
    int rsp_canonical = (frame.rsp >> 47) == 0;
    u64 rip_flags = rip_canonical ? vm64_user_flags(space, frame.rip) : 0;
    u64 stack_flags = rsp_canonical && frame.rsp > VM64_USER_BASE ?
        vm64_user_flags(space, frame.rsp - 1) : 0;
    int stack_writable = (stack_flags & VM64_PAGE_WRITE) ||
                         (stack_flags & VM64_PAGE_COW);
    if ((rip_flags & (VM64_PAGE_PRESENT | VM64_PAGE_USER | VM64_PAGE_NX)) !=
        (VM64_PAGE_PRESENT | VM64_PAGE_USER) ||
        (stack_flags & (VM64_PAGE_PRESENT | VM64_PAGE_USER)) !=
        (VM64_PAGE_PRESENT | VM64_PAGE_USER) || !stack_writable)
        return POSIX_SIGNAL_EINVAL;
    state->blocked = frame.saved_mask;
    regs->rax = frame.rax;
    regs->rcx = frame.rcx;
    regs->rdx = frame.rdx;
    regs->rsi = frame.rsi;
    regs->rdi = frame.rdi;
    regs->r8 = frame.r8;
    regs->r9 = frame.r9;
    regs->r10 = frame.r10;
    regs->r11 = frame.r11;
    regs->rip = frame.rip;
    regs->rsp = frame.rsp;
    regs->rflags = frame.rflags;
    return 0;
}

int posix_signal_child_exiting(struct task *child) {
    if (child->parent_id <= 0) return 0;
    u32 slot = PID_SLOT((u32)child->parent_id);
    if (!slot || slot >= MAX_TASKS) return 0;
    struct task *parent = &task_pool[slot];
    if (parent->id != child->parent_id ||
        parent->state == TASK_FREE || parent->state == TASK_ZOMBIE)
        return 0;
    struct posix_signal_state *state = state_for(parent);
    if (!state) return 0;
    // An ignored SIGCHLD is the POSIX auto-reap contract: no zombie, the
    // slot frees at once, and a later waitpid answers ECHILD.
    if (state->handler[POSIX_SIG_CHLD - 1u] == POSIX_SIG_IGN) return 1;
    if (state->handler[POSIX_SIG_CHLD - 1u] == POSIX_SIG_DFL) return 0;
    u64 bit = POSIX_SIGNAL_BIT(POSIX_SIG_CHLD);
    state->pending |= bit;
    if (!(state->blocked & bit)) wake_with_eintr(parent);
    return 0;
}
