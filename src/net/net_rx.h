#ifndef NET_RX_H
#define NET_RX_H

#include "types.h"
#include "object.h"

// One slot per CPU: the receive mailbox. An interface is bound to one CPU,
// and a frame that arrives while another CPU runs the driver is parked in
// that CPU's slot instead of being processed on the wrong CPU, where the
// protocol state and the parked waiters are not the ones that will see it.
// A slot is a hand-off, not a queue: it holds one frame, and a post that
// cannot claim it is processed by its caller, so the mailbox can add a
// hand-off and never a drop. The bound CPU drains the slot when the kick
// IPI lands, when its own tick runs and when it posts to itself.
#define NET_RX_SLOT_MAX 16u

typedef void (*net_rx_kick_fn)(u32 cpu);

void net_rx_init(void);
void net_rx_set_kick(net_rx_kick_fn kick);
int net_rx_post(u32 cpu, struct kernel_object *interface, u64 buffer_id,
                u32 offset, u32 length, u32 now);
u32 net_rx_drain(u32 cpu);
u64 net_rx_drains(u32 cpu);
u64 net_rx_handoffs(void);

#endif
