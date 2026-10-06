#ifndef POSIX_POLL_H
#define POSIX_POLL_H

#include "types.h"
#include "posix_abi.h"

struct task;

// One poll over a copied posix_poll_request, with the millisecond timeout
// the syscall took as its second argument and -1 for the endless wait.
// The request address is recorded by a park, and the waker patches the
// revents array there in place, the way the pipe and socket parks do.
// Returns the ready count, 0 for the timeout, or a negative errno.
i64 posix_poll(struct task *task, uptr_t request,
               const struct posix_poll_request *in, i64 timeout_ms);

// A signal aimed at a task parked in poll answers EINTR.
i64 posix_poll_signal(struct task *target);

// A readiness change on any waiter wakes the parked polls whose listed
// descriptors came ready; the pipe and socket wake paths call this once
// per event rather than tracking which poll listed what.
void posix_poll_notify(void);

// The timer tick closes the timeouts: a caller parked on a deadline has
// its dispatch frame abandoned, so the tick patches the answer in place.
void posix_poll_tick(u32 now);

#endif
