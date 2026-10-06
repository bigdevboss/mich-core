#ifndef POSIX_SIGNAL_H
#define POSIX_SIGNAL_H

#include "types.h"

struct task;

// Errno surface of the signal calls, negative like the sibling modules.
#define POSIX_SIGNAL_EPERM (-1)
#define POSIX_SIGNAL_ESRCH (-3)
#define POSIX_SIGNAL_EINTR (-4)
#define POSIX_SIGNAL_EINVAL (-22)
// Stopping the only runnable task would park the CPU with nobody left to
// send the continue, so that stop is refused instead.
#define POSIX_SIGNAL_EDEADLK (-35)

// One bit per signal number, signals 1 through 31. Signal zero is the
// existence probe and never sets a bit.
#define POSIX_SIGNAL_BIT(signo) (1u << ((signo) - 1u))

// Wipe the signal state of a slot. Called on task teardown so a recycled
// slot does not inherit a previous life's handlers or pending bits.
void posix_signal_reset(struct task *task);

// Inherit on fork: handlers, the blocked mask, and per-signal restorers
// copy to the child, pending bits do not. POSIX leaves the child's pending
// set empty.
void posix_signal_fork(struct task *parent, struct task *child);

// Reset dispositions on exec: caught handlers fall back to the default
// action, explicit ignores survive, and the blocked mask carries over.
void posix_signal_exec(struct task *task);

// The x86 vector a user fault raised, translated to the signal the profile
// reports. 0 when the vector has no signal mapping and the old terminate
// path applies.
u32 posix_signal_fault_signo(u32 vector);

// Post a signal and act on it. Returns 0 when the signal was posted or
// needs no action, 1 when the caller must terminate the target at once
// (SIGKILL, or a default-terminate disposition that is deliverable), 2
// when the target stopped itself and the caller must yield the CPU, and a
// negative errno otherwise: -EINVAL for a signal outside the bounded set
// or a non positive pid, -ESRCH when the pid names no live task, -EPERM
// when the caller's uid owns neither side. A catchable signal parks the
// bit, wakes a sleeping or waiting target with EINTR, and lets the
// delivery points run the disposition.
int posix_signal_kill(struct task *caller, int pid, u32 signo);

// One target, one signal, the disposition rules: the shape the group
// walk and the single-pid call share. The return codes are the ones
// posix_signal_kill documents.
int posix_signal_one(struct task *caller, struct task *target, u32 signo);

// Resolve a pid to a live task, or 0. Excludes the free and the zombie.
struct task *posix_signal_target(int pid);

// Whether the bounded set carries the number, which is the gate every
// entry point opens with.
int posix_signal_known(u32 signo);

// Stop and continue, the two state transitions a job-control signal owns.
// Stopping breaks a park with EINTR first, and both publish the report the
// parent collects through waitpid's WUNTRACED and WCONTINUED.
void posix_signal_stop(struct task *target, u32 signo);
void posix_signal_continue_task(struct task *target);

// The number of a pending, unblocked signal, lowest first, or 0. The
// delivery points call this to decide whether to build a frame.
u32 posix_signal_pick(struct task *task);

// Install or read one disposition. The request carries the new handler,
// restorer, and block mask in, and answers with the previous set so one
// record serves both sigaction pointers. 0 on success, -EINVAL on a
// signal the profile does not carry.
int posix_signal_action(struct task *task, u32 signo, void *request);

// Apply a sigprocmask operation. 0 on success, -EINVAL on an unknown how.
int posix_signal_procmask(struct task *task, u32 how, void *request);

// Answer with the pending signals the caller has blocked.
void posix_signal_pending(struct task *task, u64 *pending);

struct posix_signal_regs {
    u64 rax;
    u64 rcx;
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 rip;
    u64 rsp;
    u64 rflags;
};

// Deliver one pending signal: push a posix_sigframe onto the user stack
// and rewrite the register set so execution enters the handler. A nonzero
// forced signo delivers a hardware fault instead of a posted bit, which no
// blocked mask can hold back. Returns 1 when a frame was built, 0 when
// nothing was deliverable or the disposition resolved to ignore, and the
// negated signal number when the disposition terminates the task or the
// user stack refused the frame.
int posix_signal_deliver(struct task *task, struct posix_signal_regs *regs,
                         u32 forced);

// Reverse of a delivery: read the frame the restorer points at and fill
// the register set to resume. The caller hands in the address space index
// because only it can see its own context table. Returns 0, or -EINVAL
// when the frame address is not canonical or not readable, or the frame
// it carries does not resume into a mapped user code and stack page.
int posix_signal_restore(struct task *task, u32 space, uptr_t frame_address,
                         struct posix_signal_regs *regs);

// Called from terminate64 before the zombie transition. Marks SIGCHLD
// pending for a live parent and returns 1 when that parent ignores
// SIGCHLD, which means the child should be freed instead of zombied: the
// POSIX auto-reap contract for an ignored SIGCHLD.
int posix_signal_child_exiting(struct task *child);

#endif
