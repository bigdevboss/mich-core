#ifndef POSIX_PLEDGE_H
#define POSIX_PLEDGE_H

#include "types.h"

struct task;

// Errno surface, negative like the sibling modules. E2BIG comes from the
// unveil entry budget, EPERM from a widening attempt or a locked unveil.
#define POSIX_PLEDGE_EPERM (-1)
#define POSIX_PLEDGE_ENOENT (-2)
#define POSIX_PLEDGE_E2BIG (-7)
#define POSIX_PLEDGE_EACCES (-13)
#define POSIX_PLEDGE_EINVAL (-22)
#define POSIX_PLEDGE_ENOSYS (-38)

// The promise vocabulary: the OpenBSD canon trimmed to the calls this
// profile carries. Bit positions are kernel internal.
#define POSIX_PLEDGE_STDIO (1u << 0)
#define POSIX_PLEDGE_RPATH (1u << 1)
#define POSIX_PLEDGE_WPATH (1u << 2)
#define POSIX_PLEDGE_CPATH (1u << 3)
#define POSIX_PLEDGE_FATTR (1u << 4)
#define POSIX_PLEDGE_PROC (1u << 5)
#define POSIX_PLEDGE_EXEC (1u << 6)
#define POSIX_PLEDGE_UNVEIL (1u << 7)
#define POSIX_PLEDGE_ERROR (1u << 8)

// Unveil permission bits, one per permission character of unveil(2).
#define POSIX_VEIL_READ (1u << 0)
#define POSIX_VEIL_WRITE (1u << 1)
#define POSIX_VEIL_EXECUTE (1u << 2)
#define POSIX_VEIL_CREATE (1u << 3)

// Per-process unveil entry budget; the 13th distinct path answers E2BIG.
#define POSIX_PLEDGE_UNVEIL_MAX 12

// Wipe the pledge state of a slot so a recycled slot starts unrestricted.
void posix_pledge_reset(struct task *task);

// Inherit on fork: the promise mask, the exec promises, and the veil table
// all copy, so the child runs inside the sandbox the parent built.
void posix_pledge_fork(struct task *parent, struct task *child);

// Apply the exec promises on execve: when the last pledge call named a
// replacement set it takes over, otherwise the current set survives. The
// veil always survives exec; dropping it would undo the sandbox.
void posix_pledge_exec(struct task *task);

// Install or narrow promises. Either string may be NULL to leave that
// half unchanged; an empty string is a valid (empty) set. A later call
// may only remove promises. 0 on success, -EINVAL on an unknown promise
// name, -EPERM on a widening attempt.
int posix_pledge_promise(struct task *task, const char *promises,
                         const char *execpromises);

// Add one unveil rule. The first successful call drops the veil over the
// whole namespace; afterwards only the unveiled subtrees stay visible.
// unveil(NULL, NULL) locks the table forever. A repeated rule on the same
// path may only narrow its permissions. 0 on success, -ENOENT when the
// path does not resolve, -EINVAL on an unknown permission letter,
// -EPERM after a lock, without the unveil promise, or on a widening
// attempt, -E2BIG when the entry budget is spent.
int posix_pledge_unveil(struct task *task, const char *path,
                        const char *permissions);

// Syscall gate. 0 when the call is allowed, POSIX_PLEDGE_ENOSYS when the
// error promise denies it quietly, 1 when the caller must deny it loudly
// with SIGABRT.
int posix_pledge_gate(struct task *task, u32 number);

// The open gate sees what the flags ask for: read access needs rpath,
// write access wpath, O_CREAT cpath.
int posix_pledge_gate_open(struct task *task, u32 flags);

// Veil check for a path operation. leaf_need carries the POSIX_VEIL_*
// bits the operation requires of the final component, create_need the
// bits a not yet existing leaf requires of its parent directory; a
// create_need of zero means a missing path stays ENOENT. 0 when the veil
// allows the operation, -ENOENT when no rule covers the path, -EACCES
// when a covering rule lacks the permission.
int posix_pledge_veil_check(struct task *task, const char *path,
                            u32 leaf_need, u32 create_need);

#endif
