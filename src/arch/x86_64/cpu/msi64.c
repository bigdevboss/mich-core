#include "msi64.h"
#include "pci64.h"
#include "vector64.h"
#include "resource.h"
#include "object.h"
#include "klock.h"

#define MSI64_MAX 32
#define MSI64_GROUP_MAX 16

struct msi64_group {
    struct kernel_object *pci;
    u32 count;
    u32 remaining;
    u32 unmasked;
    u8 base_vector;
    u8 enabled;
    u32 active;
};

struct msi64_state {
    struct msi64_group *group;
    struct kernel_object *irq;
    u32 member;
    u8 vector;
    u32 active;
};

static struct msi64_group groups[MSI64_GROUP_MAX];
static struct msi64_state states[MSI64_MAX];
static u8 destination_id;

static struct msi64_state *state_for(u32 source) {
    if (!source || source > MSI64_MAX) return 0;
    struct msi64_state *state = &states[source - 1];
    return state->active ? state : 0;
}

static int msi_mask_locked(u32 source, int masked) {
    struct msi64_state *state = state_for(source);
    if (!state || !state->group || state->member >= 32) return -1;
    struct msi64_group *group = state->group;
    u32 bit = 1u << state->member;
    if (masked) {
        group->unmasked &= ~bit;
        if (group->enabled && pci64_msi_disable(group->pci)) return -1;
        group->enabled = 0;
        return 0;
    }
    group->unmasked |= bit;
    u32 full = group->count == 32 ? 0xFFFFFFFFu :
               (1u << group->count) - 1;
    if (group->unmasked != full || group->enabled) return 0;
    if (pci64_msi_enable_group(group->pci, group->base_vector,
                               group->count, destination_id))
        return -1;
    group->enabled = 1;
    return 0;
}

static int msi_mask(u32 source, int masked) {
    klock_acquire(&irq_program_klock);
    int result = msi_mask_locked(source, masked);
    klock_release(&irq_program_klock);
    return result;
}

static void group_clear(struct msi64_group *group) {
    if (!group || !group->active || group->remaining) return;
    if (group->enabled) pci64_msi_disable(group->pci);
    if (group->pci) object_release(group->pci);
    group->pci = 0;
    group->count = 0;
    group->unmasked = 0;
    group->base_vector = 0;
    group->enabled = 0;
    group->active = 0;
}

static void msi_release_locked(u32 source) {
    struct msi64_state *state = state_for(source);
    if (!state) return;
    struct msi64_group *group = state->group;
    if (group && group->enabled) {
        pci64_msi_disable(group->pci);
        group->enabled = 0;
    }
    if (group && state->member < 32)
        group->unmasked &= ~(1u << state->member);
    uptr_t owner = vector64_owner_locked(state->vector);
    if (owner) vector64_release_locked(state->vector, owner);
    state->group = 0;
    state->irq = 0;
    state->member = 0;
    state->vector = 0;
    state->active = 0;
    if (group && group->remaining) group->remaining--;
    group_clear(group);
}

static void msi_release(u32 source) {
    klock_acquire(&irq_program_klock);
    msi_release_locked(source);
    klock_release(&irq_program_klock);
}

int msi64_init(u8 destination_apic_id) {
    klock_acquire(&irq_program_klock);
    destination_id = destination_apic_id;
    for (u32 index = 0; index < MSI64_GROUP_MAX; index++) {
        groups[index].pci = 0;
        groups[index].count = 0;
        groups[index].remaining = 0;
        groups[index].unmasked = 0;
        groups[index].base_vector = 0;
        groups[index].enabled = 0;
        groups[index].active = 0;
    }
    for (u32 index = 0; index < MSI64_MAX; index++) {
        states[index].group = 0;
        states[index].irq = 0;
        states[index].member = 0;
        states[index].vector = 0;
        states[index].active = 0;
    }
    klock_release(&irq_program_klock);
    return irq_resource_set_backend(IRQ_CONTROLLER_MSI,
                                    msi_mask, msi_release);
}

static int msi_create_group_locked(struct kernel_object *pci, u32 count,
                                   struct kernel_object **irqs,
                                   u32 capacity) {
    const struct pci_resource *description = pci_resource_get(pci);
    if (!description || !(description->capability_flags & PCI_CAP_MSI) ||
        !irqs || !count || count > capacity || count > MSI64_MAX ||
        (count & (count - 1)) || count > pci64_msi_max_vectors(pci))
        return -1;
    struct msi64_group *group = 0;
    for (u32 index = 0; index < MSI64_GROUP_MAX; index++) {
        if (groups[index].active && groups[index].pci == pci) return -1;
        if (!groups[index].active && !group) group = &groups[index];
    }
    u32 free_states = 0;
    for (u32 index = 0; index < MSI64_MAX; index++)
        if (!states[index].active) free_states++;
    if (!group || free_states < count || object_retain(pci)) return -1;
    u8 vectors[MSI64_MAX];
    if (vector64_allocate_group_locked((uptr_t)group, count, vectors)) {
        object_release(pci);
        return -1;
    }
    group->pci = pci;
    group->count = count;
    group->remaining = 0;
    group->unmasked = 0;
    group->base_vector = vectors[0];
    group->enabled = 0;
    group->active = 1;
    u32 created = 0;
    for (u32 member = 0; member < count; member++) {
        u32 slot = MSI64_MAX;
        for (u32 index = 0; index < MSI64_MAX; index++)
            if (!states[index].active) {
                slot = index;
                break;
            }
        if (slot == MSI64_MAX) break;
        struct msi64_state *state = &states[slot];
        state->group = group;
        state->member = member;
        state->vector = vectors[member];
        state->irq = 0;
        state->active = 1;
        group->remaining++;
        struct kernel_object *irq =
            irq_resource_create_kind(IRQ_CONTROLLER_MSI, slot + 1,
                                     vectors[member], IRQ_TRIGGER_EDGE,
                                     IRQ_POLARITY_HIGH);
        if (!irq) {
            msi_release_locked(slot + 1);
            break;
        }
        state->irq = irq;
        if (vector64_transfer_locked(vectors[member], (uptr_t)group,
                                     (uptr_t)irq)) {
            object_release(irq);
            break;
        }
        irqs[created++] = irq;
    }
    if (created == count) return 0;
    for (u32 index = 0; index < created; index++) object_release(irqs[index]);
    for (u32 index = created; index < count; index++) {
        uptr_t owner = vector64_owner_locked(vectors[index]);
        if (owner == (uptr_t)group)
            vector64_release_locked(vectors[index], (uptr_t)group);
    }
    group_clear(group);
    for (u32 index = 0; index < capacity; index++) irqs[index] = 0;
    return -1;
}

int msi64_create_group(struct kernel_object *pci, u32 count,
                       struct kernel_object **irqs, u32 capacity) {
    klock_acquire(&irq_program_klock);
    int result = msi_create_group_locked(pci, count, irqs, capacity);
    klock_release(&irq_program_klock);
    return result;
}

struct kernel_object *msi64_create(struct kernel_object *pci) {
    struct kernel_object *irq = 0;
    klock_acquire(&irq_program_klock);
    int result = msi_create_group_locked(pci, 1, &irq, 1);
    klock_release(&irq_program_klock);
    return result ? 0 : irq;
}
