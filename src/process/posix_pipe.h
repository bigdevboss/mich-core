#ifndef POSIX_PIPE_H
#define POSIX_PIPE_H

#include "types.h"
#include "posix_abi.h"
#include "posix_vfs.h"

// The pipe pool is static and bounded, like the task and ofd pools: no
// hot-path allocation, and a full pool answers ENFILE.
#define POSIX_PIPE_MAX 32u
#define POSIX_PIPE_BUF 4096u

#define POSIX_PIPE_END_READ 1u
#define POSIX_PIPE_END_WRITE 2u

struct task;

// Take a pipe from the pool. Both end counts start at zero; each installed
// descriptor retains its end, so the counts mirror the end ofd references
// one for one. Returns the pool index or -ENFILE.
int posix_pipe_create(void);

// A descriptor for one end appeared (install, dup, fork) or disappeared
// (close, dup2 replace, exit). The last release of an end delivers what
// POSIX hangs off it: readers see EOF, writers see EPIPE with SIGPIPE.
void posix_pipe_retain(u32 index, u32 end);
void posix_pipe_release(u32 index, u32 end);

// One read or write on a pipe end. The request address is the user
// posix_io_request the syscall copied in; a park abandons its dispatch
// frame, so the peer that completes the operation patches the data array
// and the transferred word in that buffer directly, the way the signal
// layer patches the nanosleep remainder. Returns the byte count, 0 for
// end of file, or a negative errno.
int posix_pipe_io(struct task *task, u32 index, u32 end, uptr_t request,
                  u32 length);

// Answer a signal aimed at a task parked in posix_pipe_io: reads park only
// on an empty ring and writes are all-or-nothing under PIPE_BUF, so the
// interrupted call always owes its whole request and answers EINTR.
i64 posix_pipe_signal(struct task *target);

// The readiness one end of a pipe carries, before the caller's requested
// event set narrows it: data or a hangup on the read end, room or a dead
// peer on the write end. An index that names no live pipe answers
// POSIX_POLLNVAL.
u16 posix_pipe_poll(u32 index, u32 end);

u32 posix_pipe_active_count(void);

#endif
