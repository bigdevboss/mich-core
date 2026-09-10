#include "route.h"

static int netmask_prefix(u32 netmask, u32 *prefix) {
    u32 inverse = ~netmask;
    if (inverse & (inverse + 1)) return -1;
    u32 count = 0;
    while (netmask & 0x80000000u) {
        count++;
        netmask <<= 1;
    }
    *prefix = count;
    return 0;
}

void route_init(struct route_table *table) {
    if (!table) return;
    table->count = 0;
    for (u32 index = 0; index < ROUTE_MAX; index++) {
        table->entries[index].network = 0;
        table->entries[index].netmask = 0;
        table->entries[index].gateway = 0;
        table->entries[index].interface_id = 0;
        table->entries[index].interface_generation = 0;
        table->entries[index].metric = 0;
        table->entries[index].prefix_length = 0;
        table->entries[index].active = 0;
    }
}

int route_add_generation(struct route_table *table, u32 network, u32 netmask,
                         u32 gateway, u32 interface_id,
                         u32 interface_generation, u32 metric) {
    u32 prefix;
    if (!table || !interface_id || netmask_prefix(netmask, &prefix) ||
        network != (network & netmask))
        return -1;
    for (u32 index = 0; index < ROUTE_MAX; index++)
        if (table->entries[index].active &&
            table->entries[index].network == network &&
            table->entries[index].netmask == netmask &&
            table->entries[index].interface_id == interface_id)
            return -1;
    for (u32 index = 0; index < ROUTE_MAX; index++) {
        struct route_entry *entry = &table->entries[index];
        if (entry->active) continue;
        entry->network = network;
        entry->netmask = netmask;
        entry->gateway = gateway;
        entry->interface_id = interface_id;
        entry->interface_generation = interface_generation;
        entry->metric = metric;
        entry->prefix_length = prefix;
        entry->active = 1;
        table->count++;
        return 0;
    }
    return -1;
}

int route_add(struct route_table *table, u32 network, u32 netmask,
              u32 gateway, u32 interface_id, u32 metric) {
    return route_add_generation(table, network, netmask, gateway,
                                interface_id, 0, metric);
}

int route_remove(struct route_table *table, u32 network, u32 netmask,
                 u32 interface_id) {
    if (!table || !interface_id) return -1;
    for (u32 index = 0; index < ROUTE_MAX; index++) {
        struct route_entry *entry = &table->entries[index];
        if (!entry->active || entry->network != network ||
            entry->netmask != netmask || entry->interface_id != interface_id)
            continue;
        entry->active = 0;
        entry->network = 0;
        entry->netmask = 0;
        entry->gateway = 0;
        entry->interface_id = 0;
        entry->interface_generation = 0;
        entry->metric = 0;
        entry->prefix_length = 0;
        if (table->count) table->count--;
        return 0;
    }
    return -1;
}

const struct route_entry *route_lookup(const struct route_table *table,
                                       u32 destination) {
    if (!table) return 0;
    const struct route_entry *best = 0;
    for (u32 index = 0; index < ROUTE_MAX; index++) {
        const struct route_entry *entry = &table->entries[index];
        if (!entry->active ||
            (destination & entry->netmask) != entry->network)
            continue;
        if (!best || entry->prefix_length > best->prefix_length ||
            (entry->prefix_length == best->prefix_length &&
             entry->metric < best->metric))
            best = entry;
    }
    return best;
}

u32 route_deactivate_interface(struct route_table *table, u32 interface_id,
                               u32 interface_generation) {
    if (!table || !interface_id) return 0;
    u32 removed = 0;
    for (u32 index = 0; index < ROUTE_MAX; index++) {
        struct route_entry *entry = &table->entries[index];
        if (!entry->active || entry->interface_id != interface_id ||
            (interface_generation &&
             entry->interface_generation != interface_generation))
            continue;
        entry->active = 0;
        if (table->count) table->count--;
        removed++;
    }
    return removed;
}

u32 route_count(const struct route_table *table) {
    return table ? table->count : 0;
}

u32 route_available(const struct route_table *table) {
    if (!table) return 0;
    u32 available = 0;
    for (u32 index = 0; index < ROUTE_MAX; index++)
        if (!table->entries[index].active) available++;
    return available;
}
