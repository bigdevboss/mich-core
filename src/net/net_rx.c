#include "net_rx.h"
#include "net_interface.h"
#include "spinlock.h"


struct net_rx_slot {
    struct kernel_object *interface;
    u64 buffer_id;
    u32 offset;
    u32 length;
    u32 now;
    u32 active;
};

static struct net_rx_slot net_rx_slots[NET_RX_SLOT_MAX];
static struct spinlock net_rx_lock = SPINLOCK_INIT;
static net_rx_kick_fn net_rx_kick;
static u64 net_rx_drain_counts[NET_RX_SLOT_MAX];
static u64 net_rx_handoff_count;

void net_rx_init(void) {
    net_rx_lock.ticket = 0;
    net_rx_lock.served = 0;
    for (u32 cpu = 0; cpu < NET_RX_SLOT_MAX; cpu++) {
        net_rx_slots[cpu].interface = 0;
        net_rx_slots[cpu].active = 0;
        net_rx_drain_counts[cpu] = 0;
    }
    net_rx_handoff_count = 0;
}

void net_rx_set_kick(net_rx_kick_fn kick) {
    net_rx_kick = kick;
}

// The slot's fields and the flag are written under one lock, and the drain
// reads them the same way, so a frame is never half-visible: a post either
// fills the slot and marks it active, or finds it taken and hands back to
// its caller.
int net_rx_post(u32 cpu, struct kernel_object *interface, u64 buffer_id,
                u32 offset, u32 length, u32 now) {
    if (cpu >= NET_RX_SLOT_MAX || !interface) return 0;
    struct net_rx_slot *slot = &net_rx_slots[cpu];
    spin_lock(&net_rx_lock);
    int posted = !slot->active && !object_retain(interface);
    if (posted) {
        slot->interface = interface;
        slot->buffer_id = buffer_id;
        slot->offset = offset;
        slot->length = length;
        slot->now = now;
        slot->active = 1;
        __atomic_fetch_add(&net_rx_handoff_count, 1, __ATOMIC_RELAXED);
    }
    spin_unlock(&net_rx_lock);
    if (posted && net_rx_kick) net_rx_kick(cpu);
    return posted;
}

// The drain takes the frame under the same lock, so two CPUs that both run
// the drain deliver it once. The interface reference the post took is
// dropped after the frame is processed, outside every lock this runs under.
u32 net_rx_drain(u32 cpu) {
    if (cpu >= NET_RX_SLOT_MAX) return 0;
    struct net_rx_slot *slot = &net_rx_slots[cpu];
    struct kernel_object *interface;
    u64 buffer_id;
    u32 offset;
    u32 length;
    u32 now;
    spin_lock(&net_rx_lock);
    u32 drained = slot->active;
    if (drained) {
        interface = slot->interface;
        buffer_id = slot->buffer_id;
        offset = slot->offset;
        length = slot->length;
        now = slot->now;
        slot->interface = 0;
        slot->active = 0;
        net_rx_drain_counts[cpu]++;
    }
    spin_unlock(&net_rx_lock);
    if (!drained) return 0;
    net_interface_receive_buffer(interface, buffer_id, offset, length, now);
    object_release(interface);
    return 1;
}

u64 net_rx_drains(u32 cpu) {
    return cpu < NET_RX_SLOT_MAX ? net_rx_drain_counts[cpu] : 0;
}

u64 net_rx_handoffs(void) {
    return net_rx_handoff_count;
}
