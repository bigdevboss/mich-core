# POSIX process facade audit (step 5 + step 6)

Date: 2026-09-21
State: audit executed against uncommitted step-5/step-6 tree (HEAD `1993f45` + facade work).
Claim level: QEMU-only, reference build, single-CPU unless stated.

## Scope

Adversarial re-read of the full POSIX surface (`posix_process.c`,
`posix_profile.c`, `posix_fd.c`, `posix_vfs.c`, `posix_execve64`,
syscall dispatch gates 186..207, `wake_waiting_parent`, `reparent_children`,
`terminate64`, `task_free_slot`, `fork64`, the IF/FMASK preemption protocol,
PMM/VM refcount contracts) plus runtime stress campaigns and targeted
instrumented builds. One kernel bug was found and fixed; one open question
survives with a full exclusion trail (OQ-1).

## Fixed findings

### A1 (fixed): BSP timer path accepted ring-0 frames

`timer64_dispatch` on the BSP saved and loaded any interrupt frame through
`interrupt_save`/`interrupt_load`, which unconditionally rebuild the frame as
ring 3 (`cs=0x23`, `ss=0x1B`, `rsp=context->rsp`). A ring-0 frame has no
SS/RSP slots, so a save/load round trip corrupts the task context and the
iretq frame. The AP path already carried the `(cs & 3) == 3` guard with a
comment; the BSP path did not.

Today the guard is unreachable by protocol, not by construction: the boot
path never executes `sti` in ring 0, and `MSR_FMASK = 0x600` clears IF on
every syscall entry, so the timer can only observe ring-3 frames. Any future
`sti` in kernel code (or real AP syscalls) would have converted a routine
tick into silent context corruption.

Fix: ring-3 guard added to the BSP tick path, mirroring the AP path, with a
WHY comment recording the IF-protocol invariant. No observable behavior
change in the current configuration; verified by the full battery below.

### A2 (verified, no change): blocking-layer lost-wakeup window

`waitpid` dispatch scans for zombies, then sets `wait_pid`/`state` and
blocks; `wake_waiting_parent` (child exit path) only wakes a parent already
in `TASK_BLOCKED_WAIT`. On a truly concurrent SMP syscall path these two
sequences could interleave so the wake fires between scan and block, leaving
the parent asleep forever with an unreaped zombie. This window is closed by
the existing IF protocol (all POSIX syscalls run on the BSP with IF=0, so
parent dispatch and child termination are mutually atomic). Recorded here so
the invariant is written down; must be revisited before AP users gain real
syscall access.

## Verified contracts (no change required)

- **Admission gate**: the entire syscall range 186..207 sits behind
  `posix_profile_admitted`; non-admitted tasks receive `EACCES`. Admission
  originates only from boot metadata or fork of an admitted task.
- **execve staging**: single static scratch under one spinlock; every
  failure path releases node/space/pages; commit section (CLOEXEC close,
  native handle close, space swap, old space destroy) runs after the lock is
  dropped and cannot fail.
- **fork failure balance**: `posix_fd_fork`/`posix_profile_fork` failures
  route through `task_free_slot`, whose `arch_task_release` guards on
  `vm_valid` (zeroed on every release) — no stale-space destruction.
- **FD/OFD discipline**: retain/release never under the table lock;
  reference overflow guarded; `dup2` self-target shortcut; fork copy is
  validate-then-commit under one lock.
- **Stack builder**: page is zero-wiped per execve; pointer/arena bounds are
  re-validated; E2BIG on total > 4096.
- **PMM**: probe build with a bitmap/refs desync panic in `pmm_take_page`
  ran the full stress suite — no desync, no double allocation.

## Stress campaign (instrumented builds, not part of the commit)

- FD exhaustion: two rounds of open-to-EMFILE, close-all, unlink — 32/32,
  stable across rounds.
- Task-slot churn: fork-to-failure (13 slots on the reference image), full
  reap drain via blocking `waitpid(-1)` — 13/13 stable across rounds.
- execve argument limits (runtime): 33-element argv -> `E2BIG`; 2090-byte
  argument -> `E2BIG`; NULL argv -> `EINVAL`; missing image -> `ENOENT`;
  directory target -> `EISDIR`.
- All of the above stable when each step is traced on serial.

## OQ-1 (open): quiet fork storm corrupts the parent's saved frame

Reproducer (temporary stress code, removed): after the static application
test, in the role-1 task (the one carrying the full main() suite): two FD
exhaustion rounds, then a loop of bare `fork()` with children calling
`_exit(3)` immediately, forked until task-slot exhaustion, then a blocking
`waitpid(-1)` drain. Roughly 13 children are created with **no intervening
syscalls between forks**.

Observed, reproducible 2/2 on the quiet build, 0/6 when any serial output is
added between forks (timing-sensitive):

- The role-1 task faults at `main+0x1e` (the first role-dispatch call, after
  the 0xca8-byte `sub rsp`) with `rsp` ~0xCC0 below its last known good
  frame — i.e. it re-enters `main()` at its previous stack depth.
- Stack dump at the fault: the return address slot of an in-flight
  `mich_write` frame contains `0x100000000` (`_start`), adjacent slots zeroed
  or containing live stress locals (fork pids).
- At the fault moment: `task_contexts[1]` and the per-CPU GS frame both hold
  the last legitimate (rip, rsp) pair; the fault frame matches neither. The
  stack page has exactly one owning PTE across all live spaces; PMM refs are
  consistent (desync probe clean); no co-mapping exists at fault time.

Excluded by instrumentation: PMM double allocation, persistent co-mapping of
the stack page, corruption of `task_contexts`/GS by the save paths, ring-0
timer frames (guard now present), stale `vm_space` on failed fork, unbalanced
COW retain/release, `arch_task_release` destroying a foreign space.

Surviving hypothesis (unconfirmed): a transient co-ownership of the parent's
stack physical page during the child's first COW fault and exit, closed
before any post-fault observation point; or an ordering defect in the
child's first dispatch through the interrupt path. The decisive next probe
is a hardware-style watch on the parent stack page PTE refcount across each
child birth/death, plus a single-step trace build of the child's first
scheduling quantum.

Risk containment: the reference test suite never creates a quiet fork storm
of this shape (the native fork tests interleave other syscalls; the POSIX
demo parks its child on a blocking read before the parent proceeds), which
is why the shipped battery is green. Until OQ-1 is closed, treat "N back-to-
back forks with no intervening syscall in a POSIX task" as a known unstable
pattern, not a supported workload.

## Fix list in this tree

1. `src/arch/x86_64/kernel/kernel.c`: ring-3 guard on the BSP timer
   preemption path (A1).

## Verification evidence

After A1 and removal of all temporary instrumentation, from a clean rebuild:

- `make -j2`: clean.
- `scripts/qemu-smoke64.sh bin/x86_64/disk-test.img` (128M): PASS, all five
  POSIX application markers plus "POSIX static application pass".
- `make test64-prod`: PASS (the pre-existing prod-image hang is resolved by
  admitting init64 with the POSIX-profile bit and packing posixapp/posixdemo
  into the default/prod images).
- `make test64-unit`: PASS (status 33).
- `make test64-highmem`: PASS (768M).
- `make test64-smp`: PASS (128M).

Stability: smoke run twice consecutively post-fix, both PASS, no retries.

## OQ-1: quiet fork storm corruption (resolved)

Symptom: after `fork_burst()` reaped its 13 children, the parent faulted with
`vec=14 rip=0x1000014CE` (main+0x1e) and the canary slot at `0x1001FF2B8`
held `0x100000000` instead of the waitpid return address.

Root cause chain, confirmed with DR0-DR1 hardware watchpoints plus a
`vm64_copy_to` trace:

1. `posix_waitpid_reaped()` and `wake_waiting_parent()` stored a `u64`
   status word (8 bytes) into the user `int *status` slot, which is 4
   bytes wide. Each reaped child wrote 4 stale bytes past the slot into
   the caller's `fork_burst` frame, zeroing the low half of the saved
   `rbx` slot at `0x1001FF2E0`.
2. The high half of that slot still held `0x00000001` left by earlier
   stack users, so the corrupted slot read as `0x0000000100000000` =
   the image base (`_start`).
3. `fork_burst` epilogue `pop %rbx` loaded `_start` into `rbx`; the next
   `call *%rbx` (the `write()` report call) jumped to `_start` instead of
   `mich_write`, restarting the image mid-test with a nearly exhausted
   stack. The reentered `main` prologue pushed `rbx` = `_start` into the
   canary slot, then `sub $0xca8,%rsp` dropped `rsp` past the stack guard
   page and `call *%rbx` faulted below the stack region (`vec=14`).

Fix: both status stores now write exactly 4 bytes (`u32 status`), and the
`vm64_user_access` precheck in the waitpid path validates 4 bytes to match
the `int *status` contract.

Verification (clean rebuild, instrumentation removed):

- `scripts/qemu-smoke64.sh` 128M: PASS, all POSIX markers, stress
  `burst` stable, `syscall/sysret pass`; the OQ-1 fault is gone across
  repeated runs.
- `make test64`: PASS.
- `make release-check`: fails on the pre-existing i386 legacy smoke
  (`PANIC scratch map failed`), identical on unmodified HEAD `1993f45`;
  x86_64 targets are unaffected.
