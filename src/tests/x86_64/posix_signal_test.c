#include "types.h"
#include "posix_abi.h"
#include "posix_signal.h"
#include "task.h"
#include "tests64.h"

// The kill and disposition paths run against tasks drawn from the pool
// through task_alloc_slot, which grows the pool count rather than touching
// the slots the runner owns. Both go back through task_free_slot, so the
// probe leaves the pool the way it found it.
static struct task *borrow_slot(int *live) {
    struct task *task = task_alloc_slot();
    *live = task ? (int)(task - task_pool) : -1;
    return task;
}

int test_posix_signal64(void) {
    int valid = 1;

    // Fault vectors map onto their POSIX signals, and everything else
    // reports no mapping so the terminate path keeps its old code.
    valid = valid && posix_signal_fault_signo(0u) == POSIX_SIG_FPE;
    valid = valid && posix_signal_fault_signo(6u) == POSIX_SIG_ILL;
    valid = valid && posix_signal_fault_signo(13u) == POSIX_SIG_SEGV;
    valid = valid && posix_signal_fault_signo(14u) == POSIX_SIG_SEGV;
    valid = valid && posix_signal_fault_signo(1u) == 0u;
    valid = valid && posix_signal_fault_signo(18u) == 0u;

    // The probe doubles as the caller: a self addressed kill verdict is
    // inert inside the unit, which has no dispatcher to apply it.
    struct task *caller = 0;
    int first = -1;
    int second = -1;
    struct task *probe = borrow_slot(&first);
    struct task *peer = borrow_slot(&second);
    if (!probe || !peer || second == first) return -1;
    caller = probe;
    probe->uid = 0;
    probe->parent_id = -1;
    peer->uid = 0;
    peer->parent_id = -1;
    posix_signal_reset(probe);
    posix_signal_reset(peer);

    // Rejections: a non positive pid has no group to mean, a signal
    // outside the bounded set is EINVAL, and a dead pid is ESRCH.
    valid = valid &&
        posix_signal_kill(caller, 0, POSIX_SIG_TERM) == POSIX_SIGNAL_EINVAL;
    valid = valid &&
        posix_signal_kill(caller, -1, POSIX_SIG_TERM) == POSIX_SIGNAL_EINVAL;
    valid = valid &&
        posix_signal_kill(caller, probe->id, 5u) == POSIX_SIGNAL_EINVAL;
    valid = valid &&
        posix_signal_kill(caller, probe->id, 32u) == POSIX_SIGNAL_EINVAL;
    valid = valid &&
        posix_signal_kill(caller, 3999, POSIX_SIG_TERM) == POSIX_SIGNAL_ESRCH;
    // Signal zero is the existence probe: it acts and reports nothing.
    valid = valid && posix_signal_kill(caller, probe->id, 0u) == 0;
    // SIGKILL always answers with the terminate verdict, and a default
    // terminate disposition does the same while it is deliverable.
    valid = valid && posix_signal_kill(caller, probe->id, POSIX_SIG_KILL) == 1;
    valid = valid && posix_signal_kill(caller, probe->id, POSIX_SIG_TERM) == 1;
    // SIGCHLD default is ignore, so it neither terminates nor parks.
    valid = valid && posix_signal_kill(caller, probe->id, POSIX_SIG_CHLD) == 0;

    // A blocked default terminate signal parks instead of killing, and
    // sigpending sees it; opening the mask lets the kill verdict out.
    struct posix_sigprocmask_request mask;
    mask.how = POSIX_SIG_BLOCK;
    mask.reserved = 0;
    mask.mask = POSIX_SIGNAL_BIT(POSIX_SIG_TERM);
    mask.previous = 0;
    valid = valid && posix_signal_procmask(probe, POSIX_SIG_BLOCK, &mask) == 0;
    valid = valid && mask.previous == 0;
    valid = valid && posix_signal_kill(caller, probe->id, POSIX_SIG_TERM) == 0;
    u64 pending = 99;
    posix_signal_pending(probe, &pending);
    valid = valid && pending == POSIX_SIGNAL_BIT(POSIX_SIG_TERM);
    // The blocked mask never holds SIGKILL, whatever was asked.
    mask.mask = POSIX_SIGNAL_BIT(POSIX_SIG_KILL);
    valid = valid && posix_signal_procmask(probe, POSIX_SIG_BLOCK, &mask) == 0;
    u64 still_pending = 0;
    posix_signal_pending(probe, &still_pending);
    valid = valid && still_pending == POSIX_SIGNAL_BIT(POSIX_SIG_TERM);
    valid = valid && posix_signal_kill(caller, probe->id, POSIX_SIG_KILL) == 1;
    // The mask now holds only the term bit: the kill ask was dropped, so
    // an unblock of nothing still answers with that previous state.
    mask.mask = 0;
    valid = valid &&
        posix_signal_procmask(probe, POSIX_SIG_UNBLOCK, &mask) == 0;
    valid = valid &&
        mask.previous == POSIX_SIGNAL_BIT(POSIX_SIG_TERM);

    // SIGACTION: install reads back as the previous disposition, and the
    // untouchable signal rejects the call.
    struct posix_sigaction_request action;
    for (u32 index = 0; index < sizeof(action); index++)
        ((u8 *)&action)[index] = 0;
    action.signo = (i32)POSIX_SIG_USR1;
    action.flags = POSIX_SA_APPLY | POSIX_SA_RESTORER;
    action.handler = 0x1000;
    action.restorer = 0x2000;
    action.mask = POSIX_SIGNAL_BIT(POSIX_SIG_TERM);
    valid = valid &&
        posix_signal_action(probe, POSIX_SIG_USR1, &action) == 0;
    valid = valid && action.previous_handler == POSIX_SIG_DFL &&
        action.previous_restorer == 0 && action.previous_mask == 0 &&
        action.previous_flags == 0;
    action.handler = 0x3000;
    valid = valid &&
        posix_signal_action(probe, POSIX_SIG_USR1, &action) == 0;
    valid = valid && action.previous_handler == 0x1000 &&
        action.previous_restorer == 0x2000 &&
        action.previous_mask == POSIX_SIGNAL_BIT(POSIX_SIG_TERM) &&
        action.previous_flags == POSIX_SA_RESTORER;
    valid = valid &&
        posix_signal_action(probe, POSIX_SIG_KILL, &action) ==
        POSIX_SIGNAL_EINVAL;

    // Delivery with a caught handler takes the park, a caught SIGCHLD
    // parks the parent bit, and an ignored SIGCHLD answers auto-reap.
    valid = valid && posix_signal_kill(caller, probe->id, POSIX_SIG_USR1) == 0;
    valid = valid && posix_signal_pick(probe) == POSIX_SIG_USR1;
    // The frame push itself needs a mapped user stack, which a borrowed
    // slot does not have: the demo covers the real delivery. Here an
    // ignore retires the parked bit and a delivery answers nothing.
    struct posix_signal_regs regs;
    action.flags = POSIX_SA_APPLY;
    action.handler = POSIX_SIG_IGN;
    action.restorer = 0;
    valid = valid &&
        posix_signal_action(probe, POSIX_SIG_USR1, &action) == 0;
    valid = valid && posix_signal_kill(caller, probe->id, POSIX_SIG_USR1) == 0;
    valid = valid && posix_signal_deliver(probe, &regs, 0u) == 0 &&
        posix_signal_pick(probe) == 0;

    // child_exiting: an ignored SIGCHLD reaps, a caught one parks the bit
    // and breaks a parent park, the default reports nothing.
    action.signo = (i32)POSIX_SIG_CHLD;
    action.flags = POSIX_SA_APPLY;
    action.handler = POSIX_SIG_IGN;
    valid = valid && posix_signal_action(peer, POSIX_SIG_CHLD, &action) == 0;
    probe->parent_id = peer->id;
    valid = valid && posix_signal_child_exiting(probe) == 1;
    action.handler = 0x5000;
    action.restorer = 0x6000;
    valid = valid && posix_signal_action(peer, POSIX_SIG_CHLD, &action) == 0;
    valid = valid && posix_signal_child_exiting(probe) == 0;
    valid = valid && posix_signal_pick(peer) == POSIX_SIG_CHLD;
    posix_signal_reset(peer);
    action.handler = POSIX_SIG_DFL;
    valid = valid && posix_signal_action(peer, POSIX_SIG_CHLD, &action) == 0;
    valid = valid && posix_signal_child_exiting(probe) == 0;
    valid = valid && posix_signal_pick(peer) == 0;

    // Fork copies the dispositions and drops pending bits; exec folds
    // caught handlers back to the default and keeps explicit ignores.
    action.signo = (i32)POSIX_SIG_USR2;
    action.flags = POSIX_SA_APPLY;
    action.handler = 0x7000;
    action.restorer = 0x8000;
    action.mask = POSIX_SIGNAL_BIT(POSIX_SIG_INT);
    valid = valid &&
        posix_signal_action(probe, POSIX_SIG_USR2, &action) == 0;
    action.signo = (i32)POSIX_SIG_HUP;
    action.flags = POSIX_SA_APPLY;
    action.handler = POSIX_SIG_IGN;
    valid = valid &&
        posix_signal_action(probe, POSIX_SIG_HUP, &action) == 0;
    valid = valid && posix_signal_kill(caller, probe->id, POSIX_SIG_USR2) == 0;
    posix_signal_fork(probe, peer);
    struct posix_sigaction_request readback;
    for (u32 index = 0; index < sizeof(readback); index++)
        ((u8 *)&readback)[index] = 0;
    readback.signo = (i32)POSIX_SIG_USR2;
    readback.handler = 0;
    valid = valid && posix_signal_action(peer, POSIX_SIG_USR2, &readback) == 0;
    valid = valid && readback.previous_handler == 0x7000 &&
        readback.previous_restorer == 0x8000 &&
        readback.previous_mask == POSIX_SIGNAL_BIT(POSIX_SIG_INT);
    valid = valid && posix_signal_pick(peer) == 0;
    posix_signal_exec(peer);
    readback.previous_handler = 0;
    valid = valid && posix_signal_action(peer, POSIX_SIG_USR2, &readback) == 0;
    valid = valid && readback.previous_handler == POSIX_SIG_DFL;
    readback.signo = (i32)POSIX_SIG_HUP;
    readback.previous_handler = 0;
    valid = valid && posix_signal_action(peer, POSIX_SIG_HUP, &readback) == 0;
    valid = valid && readback.previous_handler == POSIX_SIG_IGN;

    posix_signal_reset(probe);
    posix_signal_reset(peer);
    task_free_slot(probe);
    task_free_slot(peer);
    return valid ? 0 : -1;
}
