#ifndef POSIX_PGROUP_H
#define POSIX_PGROUP_H

#include "types.h"

struct task;

// Process groups, one group id per task slot, the way the signal and fd
// layers keep their per-slot state beside the task struct. The profile has
// a single session, so a group id is just the pid of the process that
// created the group, and a fork inherits the parent's group the way exec
// keeps it.

void posix_pgroup_init(void);

// The slot's group id; a slot that never called setpgid reports the pid it
// was created with, which is the POSIX "own group" default.
u32 posix_pgroup_get(struct task *task);

int posix_pgroup_set(struct task *task, u32 pgid);
int posix_pgroup_fork(struct task *parent, struct task *child);
void posix_pgroup_reset(struct task *task);

// True when some live task belongs to the group, which is the only
// meaning a group id has here: there is no separate group object to
// outlive its members.
int posix_pgroup_live(u32 pgid);

// Collect the live members of a group, bounded by capacity; the caller
// signals them one by one so a handler that stops or exits its own task
// cannot break the walk.
u32 posix_pgroup_members(u32 pgid, struct task **members, u32 capacity);

// The group a caller joins when it names nobody: its own.
u32 posix_pgroup_id_of(struct task *task);

#endif
