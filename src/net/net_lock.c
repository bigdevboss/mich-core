#include "net_lock.h"
#include "klock.h"
#include "scheduler.h"
#include "protos.h"

// Recursive on purpose. The receive path reaches the socket layer from
// inside the trap frame, and the syscall path reaches it from the task's
// own call, and neither can ask the other what it already holds: the depth
// count keeps the two from tripping the order checker over the same lock.
// What the recursion must not hide is a forgetting unlock, so an unbounded
// depth aborts instead of turning the lock into a no-op.
#define NET_LOCK_DEPTH_MAX 8u

static struct klock net_klock = KLOCK_INIT(KLOCK_LEVEL_NET);
static u32 net_lock_depth[KLOCK_MAX_CPUS];

static u32 net_lock_cpu(void) {
    int cpu = scheduler_cpu_id();
    return cpu > 0 && (u32)cpu < KLOCK_MAX_CPUS ? (u32)cpu : 0;
}

void net_lock(void) {
    u32 cpu = net_lock_cpu();
    if (net_lock_depth[cpu] >= NET_LOCK_DEPTH_MAX)
        panic_str("NET lock depth");
    if (net_lock_depth[cpu]++) return;
    klock_acquire(&net_klock);
}

void net_unlock(void) {
    u32 cpu = net_lock_cpu();
    if (!net_lock_depth[cpu]) return;
    if (--net_lock_depth[cpu]) return;
    klock_release(&net_klock);
}
