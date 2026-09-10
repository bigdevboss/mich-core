#ifndef ROUTE_H
#define ROUTE_H

#include "types.h"

#define ROUTE_MAX 32
#define ROUTE_INTERFACE_LOOPBACK 1
#define ROUTE_INTERFACE_VNIC 2

struct route_entry {
    u32 network;
    u32 netmask;
    u32 gateway;
    u32 interface_id;
    u32 interface_generation;
    u32 metric;
    u32 prefix_length;
    u32 active;
};

struct route_table {
    struct route_entry entries[ROUTE_MAX];
    u32 count;
};

void route_init(struct route_table *table);
int route_add(struct route_table *table, u32 network, u32 netmask,
              u32 gateway, u32 interface_id, u32 metric);
int route_add_generation(struct route_table *table, u32 network, u32 netmask,
                         u32 gateway, u32 interface_id,
                         u32 interface_generation, u32 metric);
int route_remove(struct route_table *table, u32 network, u32 netmask,
                 u32 interface_id);
const struct route_entry *route_lookup(const struct route_table *table,
                                       u32 destination);
u32 route_deactivate_interface(struct route_table *table, u32 interface_id,
                               u32 interface_generation);
u32 route_count(const struct route_table *table);
u32 route_available(const struct route_table *table);

#endif
