#ifndef NET_LOCK_H
#define NET_LOCK_H

// The coarse lock the S2 model puts on the net stack: the interface table,
// the per-interface protocol state and the socket table. The RX path, the
// tick and the driver syscalls can run on three CPUs at once, and the lock
// is what keeps them apart until S3 splits it. The order it sits in is
// written down in the notes, level NET.
void net_lock(void);
void net_unlock(void);

#endif
