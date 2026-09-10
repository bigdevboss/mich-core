#include "pmtu.h"

static int address6_equal(const u8 left[16], const u8 right[16]) {
    if (!left || !right) return 0;
    for (u32 index = 0; index < 16; index++)
        if (left[index] != right[index]) return 0;
    return 1;
}

static int address6_unspecified(const u8 address[16]) {
    if (!address) return 1;
    for (u32 index = 0; index < 16; index++)
        if (address[index]) return 0;
    return 1;
}

static u32 clamp_mtu(u32 mtu, u32 ceiling, u32 minimum) {
    if (!mtu) mtu = minimum;
    if (mtu > ceiling) mtu = ceiling;
    if (mtu < minimum) mtu = minimum;
    if (mtu > ceiling) mtu = ceiling;
    return mtu;
}

void pmtu_init(struct pmtu_cache *cache, u32 ttl) {
    if (!cache || !ttl) return;
    cache->now = 0;
    cache->ttl = ttl;
    cache->updates = 0;
    cache->hits = 0;
    cache->expired = 0;
    for (u32 index = 0; index < PMTU_ENTRY_MAX; index++) {
        struct pmtu_entry *entry = &cache->entries[index];
        entry->family = 0;
        entry->address4 = 0;
        for (u32 byte = 0; byte < 16; byte++) entry->address6[byte] = 0;
        entry->mtu = 0;
        entry->expires = 0;
        entry->active = 0;
    }
}

void pmtu_tick(struct pmtu_cache *cache, u32 now) {
    if (!cache) return;
    cache->now = now;
    for (u32 index = 0; index < PMTU_ENTRY_MAX; index++) {
        struct pmtu_entry *entry = &cache->entries[index];
        if (!entry->active || (i32)(now - entry->expires) < 0) continue;
        entry->active = 0;
        cache->expired++;
    }
}

static struct pmtu_entry *find4(struct pmtu_cache *cache, u32 destination) {
    for (u32 index = 0; index < PMTU_ENTRY_MAX; index++) {
        struct pmtu_entry *entry = &cache->entries[index];
        if (entry->active && entry->family == 4 &&
            entry->address4 == destination)
            return entry;
    }
    return 0;
}

static struct pmtu_entry *find6(struct pmtu_cache *cache,
                                const u8 destination[16]) {
    for (u32 index = 0; index < PMTU_ENTRY_MAX; index++) {
        struct pmtu_entry *entry = &cache->entries[index];
        if (entry->active && entry->family == 6 &&
            address6_equal(entry->address6, destination))
            return entry;
    }
    return 0;
}

static struct pmtu_entry *allocate_entry(struct pmtu_cache *cache) {
    struct pmtu_entry *oldest = 0;
    for (u32 index = 0; index < PMTU_ENTRY_MAX; index++) {
        struct pmtu_entry *entry = &cache->entries[index];
        if (!entry->active) return entry;
        if (!oldest || (i32)(entry->expires - oldest->expires) < 0)
            oldest = entry;
    }
    return oldest;
}

static int store(struct pmtu_cache *cache, struct pmtu_entry *entry,
                 u32 mtu) {
    if (entry->active && mtu >= entry->mtu) {
        entry->expires = cache->now + cache->ttl;
        return 1;
    }
    entry->mtu = mtu;
    entry->expires = cache->now + cache->ttl;
    entry->active = 1;
    cache->updates++;
    return 0;
}

int pmtu_update4(struct pmtu_cache *cache, u32 destination, u32 mtu,
                 u32 ceiling) {
    if (!cache || !destination || !ceiling) return -1;
    mtu = clamp_mtu(mtu, ceiling, PMTU_IPV4_MIN);
    struct pmtu_entry *entry = find4(cache, destination);
    if (!entry) {
        entry = allocate_entry(cache);
        if (!entry) return -1;
        entry->family = 4;
        entry->address4 = destination;
        for (u32 byte = 0; byte < 16; byte++) entry->address6[byte] = 0;
    }
    return store(cache, entry, mtu);
}

int pmtu_update6(struct pmtu_cache *cache, const u8 destination[16],
                 u32 mtu, u32 ceiling) {
    if (!cache || address6_unspecified(destination) ||
        destination[0] == 0xFF || !ceiling)
        return -1;
    mtu = clamp_mtu(mtu, ceiling, PMTU_IPV6_MIN);
    struct pmtu_entry *entry = find6(cache, destination);
    if (!entry) {
        entry = allocate_entry(cache);
        if (!entry) return -1;
        entry->family = 6;
        entry->address4 = 0;
        for (u32 byte = 0; byte < 16; byte++)
            entry->address6[byte] = destination[byte];
    }
    return store(cache, entry, mtu);
}

u32 pmtu_lookup4(struct pmtu_cache *cache, u32 destination, u32 ceiling) {
    if (!cache || !destination || !ceiling) return ceiling;
    struct pmtu_entry *entry = find4(cache, destination);
    if (!entry) return ceiling;
    if ((i32)(cache->now - entry->expires) >= 0) {
        entry->active = 0;
        cache->expired++;
        return ceiling;
    }
    cache->hits++;
    return entry->mtu < ceiling ? entry->mtu : ceiling;
}

u32 pmtu_lookup6(struct pmtu_cache *cache, const u8 destination[16],
                 u32 ceiling) {
    if (!cache || address6_unspecified(destination) || !ceiling)
        return ceiling;
    struct pmtu_entry *entry = find6(cache, destination);
    if (!entry) return ceiling;
    if ((i32)(cache->now - entry->expires) >= 0) {
        entry->active = 0;
        cache->expired++;
        return ceiling;
    }
    cache->hits++;
    return entry->mtu < ceiling ? entry->mtu : ceiling;
}
