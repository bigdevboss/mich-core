#ifndef PMTU_H
#define PMTU_H

#include "types.h"

#define PMTU_ENTRY_MAX 16
#define PMTU_IPV4_MIN 576
#define PMTU_IPV6_MIN 1280

struct pmtu_entry {
    u32 family;
    u32 address4;
    u8 address6[16];
    u32 mtu;
    u32 expires;
    u32 active;
};

struct pmtu_cache {
    struct pmtu_entry entries[PMTU_ENTRY_MAX];
    u32 now;
    u32 ttl;
    u64 updates;
    u64 hits;
    u64 expired;
};

void pmtu_init(struct pmtu_cache *cache, u32 ttl);
void pmtu_tick(struct pmtu_cache *cache, u32 now);
int pmtu_update4(struct pmtu_cache *cache, u32 destination, u32 mtu,
                 u32 ceiling);
int pmtu_update6(struct pmtu_cache *cache, const u8 destination[16],
                 u32 mtu, u32 ceiling);
u32 pmtu_lookup4(struct pmtu_cache *cache, u32 destination, u32 ceiling);
u32 pmtu_lookup6(struct pmtu_cache *cache, const u8 destination[16],
                 u32 ceiling);

#endif
