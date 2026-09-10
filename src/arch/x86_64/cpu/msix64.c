#include "msix64.h"
#include "pci64.h"
#include "vector64.h"
#include "vm64.h"
#include "resource.h"
#include "object.h"

#define MSIX64_MAX 64

struct msix64_state {
    struct kernel_object *table;
    struct kernel_object *irq;
    void *mapping;
    usize_t mapping_length;
    volatile u32 *entry;
    u32 entry_index;
    u8 vector;
    u8 enabled;
    u32 active;
};

static struct msix64_state states[MSIX64_MAX];
static u8 destination_id;

static struct msix64_state *state_for(u32 source) {
    if (!source || source > MSIX64_MAX) return 0;
    struct msix64_state *state = &states[source - 1];
    return state->active ? state : 0;
}

static int msix_mask(u32 source, int masked) {
    struct msix64_state *state = state_for(source);
    if (!state || !state->entry) return -1;
    if (!masked) {
        const struct msix_table_resource *table =
            msix_table_resource_get(state->table);
        if (!table || pci64_msix_configure(table->pci, 1, 0)) return -1;
        state->entry[3] &= ~1u;
        state->enabled = 1;
    } else {
        state->entry[3] |= 1u;
        state->enabled = 0;
    }
    return 0;
}

static void msix_release(u32 source) {
    struct msix64_state *state = state_for(source);
    if (!state) return;
    if (state->entry) state->entry[3] |= 1u;
    struct kernel_object *pci = 0;
    const struct msix_table_resource *table =
        msix_table_resource_get(state->table);
    if (table && !object_retain(table->pci)) pci = table->pci;
    if (state->mapping)
        vm64_iounmap(state->mapping, state->mapping_length);
    uptr_t owner = vector64_owner(state->vector);
    if (owner) vector64_release(state->vector, owner);
    if (state->table) object_release(state->table);
    state->table = 0;
    state->irq = 0;
    state->mapping = 0;
    state->mapping_length = 0;
    state->entry = 0;
    state->entry_index = 0;
    state->vector = 0;
    state->enabled = 0;
    state->active = 0;
    if (pci) {
        int remaining = 0;
        for (u32 index = 0; index < MSIX64_MAX; index++) {
            if (!states[index].active) continue;
            const struct msix_table_resource *other =
                msix_table_resource_get(states[index].table);
            if (other && other->pci == pci) remaining = 1;
        }
        if (!remaining) pci64_msix_configure(pci, 0, 1);
        object_release(pci);
    }
}

int msix64_init(u8 destination_apic_id) {
    destination_id = destination_apic_id;
    for (u32 index = 0; index < MSIX64_MAX; index++) {
        states[index].table = 0;
        states[index].irq = 0;
        states[index].mapping = 0;
        states[index].mapping_length = 0;
        states[index].entry = 0;
        states[index].entry_index = 0;
        states[index].vector = 0;
        states[index].enabled = 0;
        states[index].active = 0;
    }
    return irq_resource_set_backend(IRQ_CONTROLLER_MSIX,
                                    msix_mask, msix_release);
}

struct kernel_object *msix64_create(struct kernel_object *table_object,
                                    u32 entry_index) {
    const struct msix_table_resource *table =
        msix_table_resource_get(table_object);
    if (!table || entry_index >= table->entries) return 0;
    const struct pci_resource *pci = pci_resource_get(table->pci);
    if (!pci || table->table_bir >= 6) return 0;
    const struct pci_bar_resource *bar = &pci->bars[table->table_bir];
    u64 entry_offset = (u64)table->table_offset + (u64)entry_index * 16;
    if (!bar->address || !bar->length || entry_offset > bar->length ||
        16 > bar->length - entry_offset)
        return 0;
    for (u32 index = 0; index < MSIX64_MAX; index++) {
        if (!states[index].active || states[index].entry_index != entry_index)
            continue;
        const struct msix_table_resource *other =
            msix_table_resource_get(states[index].table);
        if (other && other->pci == table->pci) return 0;
    }
    for (u32 index = 0; index < MSIX64_MAX; index++) {
        struct msix64_state *state = &states[index];
        if (state->active) continue;
        u64 physical = bar->address + entry_offset;
        paddr_t page = physical & ~0xFFFULL;
        usize_t offset = physical & 0xFFF;
        usize_t length = (offset + 16 + 0xFFF) & ~(usize_t)0xFFF;
        void *mapping = vm64_ioremap(page, length, VM64_CACHE_UC);
        if (!mapping) return 0;
        if (object_retain(table_object)) {
            vm64_iounmap(mapping, length);
            return 0;
        }
        state->table = table_object;
        state->mapping = mapping;
        state->mapping_length = length;
        state->entry = (volatile u32 *)(uptr_t)
            ((uptr_t)mapping + offset);
        state->entry_index = entry_index;
        state->irq = 0;
        state->enabled = 0;
        state->active = 1;
        int vector = vector64_allocate((uptr_t)state);
        if (vector < 0) {
            msix_release(index + 1);
            return 0;
        }
        state->vector = (u8)vector;
        state->entry[3] |= 1u;
        state->entry[0] = 0xFEE00000u | ((u32)destination_id << 12);
        state->entry[1] = 0;
        state->entry[2] = (u32)vector;
        struct kernel_object *irq =
            irq_resource_create_kind(IRQ_CONTROLLER_MSIX, index + 1,
                                     (u32)vector, IRQ_TRIGGER_EDGE,
                                     IRQ_POLARITY_HIGH);
        if (!irq) {
            msix_release(index + 1);
            return 0;
        }
        state->irq = irq;
        if (vector64_transfer((u8)vector, (uptr_t)state, (uptr_t)irq)) {
            object_release(irq);
            return 0;
        }
        return irq;
    }
    return 0;
}

int msix64_irq_entry(struct kernel_object *irq, struct kernel_object *pci,
                     u32 *entry_index) {
    if (!irq || !pci || !entry_index) return -1;
    for (u32 index = 0; index < MSIX64_MAX; index++) {
        struct msix64_state *state = &states[index];
        if (!state->active || state->irq != irq) continue;
        const struct msix_table_resource *table =
            msix_table_resource_get(state->table);
        if (!table || table->pci != pci) return -1;
        *entry_index = state->entry_index;
        return 0;
    }
    return -1;
}

int msix64_create_group(struct kernel_object *table, u32 first_entry,
                        u32 count, struct kernel_object **irqs,
                        u32 capacity) {
    const struct msix_table_resource *resource =
        msix_table_resource_get(table);
    if (!resource || !irqs || !count || count > capacity ||
        count > MSIX64_MAX || first_entry >= resource->entries ||
        count > resource->entries - first_entry)
        return -1;
    u32 created = 0;
    while (created < count) {
        struct kernel_object *irq =
            msix64_create(table, first_entry + created);
        if (!irq) break;
        irqs[created++] = irq;
    }
    if (created == count) return 0;
    while (created) object_release(irqs[--created]);
    for (u32 index = 0; index < capacity; index++) irqs[index] = 0;
    return -1;
}
