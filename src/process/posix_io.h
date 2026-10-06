#ifndef POSIX_IO_H
#define POSIX_IO_H

#include "types.h"

// The parked dispatch frame is abandoned on the switch, so whoever
// completes the operation patches the parked caller's request in place:
// the transferred word and the data array sit at these fixed offsets from
// the request the caller handed the syscall.
#define POSIX_IO_TRANSFERRED_OFFSET 8u
#define POSIX_IO_DATA_OFFSET 12u

struct task;

// Publish the byte count into a parked or running caller's request. The
// answer is negative when the caller's memory cannot be written, which the
// completing side reports as EIO.
int posix_io_patch_result(struct task *task, uptr_t request, u32 transferred);

#endif
