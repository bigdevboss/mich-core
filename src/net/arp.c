#include "arp.h"

static u16 read_be16(const u8 *bytes) {
    return (u16)((u16)bytes[0] << 8) | bytes[1];
}

static u32 read_be32(const u8 *bytes) {
    return ((u32)bytes[0] << 24) | ((u32)bytes[1] << 16) |
           ((u32)bytes[2] << 8) | bytes[3];
}

static void write_be16(u8 *bytes, u16 value) {
    bytes[0] = (u8)(value >> 8);
    bytes[1] = (u8)value;
}

static void write_be32(u8 *bytes, u32 value) {
    bytes[0] = (u8)(value >> 24);
    bytes[1] = (u8)(value >> 16);
    bytes[2] = (u8)(value >> 8);
    bytes[3] = (u8)value;
}

static int address_equal(const u8 *left, const u8 *right) {
    for (u32 index = 0; index < 6; index++)
        if (left[index] != right[index]) return 0;
    return 1;
}

static int hardware_valid(const u8 *address) {
    if (!address || (address[0] & 1)) return 0;
    int nonzero = 0;
    for (u32 index = 0; index < 6; index++)
        if (address[index]) nonzero = 1;
    return nonzero;
}

static int protocol_valid(u32 address) {
    u8 first = (u8)(address >> 24);
    return address && address != 0xFFFFFFFFu && first != 0 &&
           first != 127 && first < 224;
}

static struct arp_cache_entry *find_entry(struct arp_context *arp,
                                          u32 address) {
    for (u32 index = 0; index < ARP_CACHE_MAX; index++)
        if (arp->cache[index].state != ARP_CACHE_EMPTY &&
            arp->cache[index].protocol_address == address)
            return &arp->cache[index];
    return 0;
}

static struct arp_cache_entry *allocate_entry(struct arp_context *arp,
                                              u32 address, u32 now) {
    struct arp_cache_entry *oldest = 0;
    for (u32 index = 0; index < ARP_CACHE_MAX; index++) {
        struct arp_cache_entry *entry = &arp->cache[index];
        if (entry->state == ARP_CACHE_EMPTY) {
            oldest = entry;
            break;
        }
        if (!oldest || (i32)(entry->updated - oldest->updated) < 0)
            oldest = entry;
    }
    if (!oldest) return 0;
    oldest->protocol_address = address;
    for (u32 index = 0; index < 6; index++) oldest->hardware_address[index] = 0;
    oldest->updated = now;
    oldest->expires = now + arp->cache_ttl;
    oldest->last_request = now - arp->request_interval;
    oldest->state = ARP_CACHE_PENDING;
    return oldest;
}

static void learn(struct arp_context *arp, u32 address,
                  const u8 hardware[6], u32 now) {
    struct arp_cache_entry *entry = find_entry(arp, address);
    if (!entry) entry = allocate_entry(arp, address, now);
    if (!entry) return;
    entry->protocol_address = address;
    for (u32 index = 0; index < 6; index++)
        entry->hardware_address[index] = hardware[index];
    entry->updated = now;
    entry->expires = now + arp->cache_ttl;
    entry->state = ARP_CACHE_REACHABLE;
    arp->stats.cache_updates++;
    if (arp->update) arp->update(address, hardware, arp->update_context);
}

static void build_packet(u8 packet[ARP_PACKET_SIZE], u16 operation,
                         const u8 sender_hardware[6], u32 sender_protocol,
                         const u8 target_hardware[6], u32 target_protocol) {
    write_be16(packet, 1);
    write_be16(packet + 2, 0x0800);
    packet[4] = 6;
    packet[5] = 4;
    write_be16(packet + 6, operation);
    for (u32 index = 0; index < 6; index++) {
        packet[8 + index] = sender_hardware[index];
        packet[18 + index] = target_hardware[index];
    }
    write_be32(packet + 14, sender_protocol);
    write_be32(packet + 24, target_protocol);
}

int arp_init(struct arp_context *arp, struct ethernet_port *port,
             u32 local_address, u32 cache_ttl, u32 request_interval,
             arp_transmit_fn transmit, void *transmit_context) {
    if (!arp || !port || !protocol_valid(local_address) || !cache_ttl ||
        cache_ttl > 0x7FFFFFFFu || !request_interval ||
        request_interval > cache_ttl || !transmit)
        return -1;
    arp->port = port;
    arp->local_address = local_address;
    arp->cache_ttl = cache_ttl;
    arp->request_interval = request_interval;
    arp->now = 0;
    arp->reply_window_start = 0;
    arp->replies_in_window = 0;
    arp->transmit = transmit;
    arp->transmit_context = transmit_context;
    arp->update = 0;
    arp->update_context = 0;
    for (u32 index = 0; index < ARP_CACHE_MAX; index++) {
        arp->cache[index].protocol_address = 0;
        for (u32 byte = 0; byte < 6; byte++)
            arp->cache[index].hardware_address[byte] = 0;
        arp->cache[index].updated = 0;
        arp->cache[index].expires = 0;
        arp->cache[index].last_request = 0;
        arp->cache[index].state = ARP_CACHE_EMPTY;
    }
    u8 *stats = (u8 *)&arp->stats;
    for (usize_t index = 0; index < sizeof(arp->stats); index++) stats[index] = 0;
    return ethernet_register_handler(port, 0x0806,
                                     arp_ethernet_handler, arp);
}

int arp_set_update_callback(struct arp_context *arp,
                            arp_neighbor_update_fn update, void *context) {
    if (!arp || !update) return -1;
    arp->update = update;
    arp->update_context = context;
    return 0;
}

int arp_set_local_address(struct arp_context *arp, u32 local_address) {
    if (!arp || !protocol_valid(local_address)) return -1;
    arp->local_address = local_address;
    for (u32 index = 0; index < ARP_CACHE_MAX; index++)
        arp->cache[index].state = ARP_CACHE_EMPTY;
    return 0;
}

int arp_ethernet_handler(struct ethernet_port *port,
                         const struct ethernet_frame_view *frame,
                         void *context) {
    struct arp_context *arp = (struct arp_context *)context;
    if (!port || !frame || !arp || arp->port != port ||
        frame->ether_type != 0x0806 || frame->payload_length < ARP_PACKET_SIZE) {
        if (arp) arp->stats.malformed++;
        return -1;
    }
    const u8 *packet = frame->payload;
    u16 operation = read_be16(packet + 6);
    const u8 *sender_hardware = packet + 8;
    u32 sender_protocol = read_be32(packet + 14);
    const u8 *target_hardware = packet + 18;
    u32 target_protocol = read_be32(packet + 24);
    if (read_be16(packet) != 1 || read_be16(packet + 2) != 0x0800 ||
        packet[4] != 6 || packet[5] != 4 ||
        (operation != 1 && operation != 2) ||
        !hardware_valid(sender_hardware) ||
        !address_equal(sender_hardware, frame->frame + 6) ||
        !protocol_valid(sender_protocol) || !protocol_valid(target_protocol)) {
        arp->stats.malformed++;
        return -1;
    }
    if (sender_protocol == arp->local_address &&
        !address_equal(sender_hardware, port->address)) {
        arp->stats.conflicts++;
        return -1;
    }
    if (operation == 1) {
        arp->stats.requests_received++;
        if (target_protocol != arp->local_address) return 0;
        learn(arp, sender_protocol, sender_hardware, arp->now);
        if ((i32)(arp->now - arp->reply_window_start) >=
            (i32)ARP_REPLY_WINDOW) {
            arp->reply_window_start = arp->now;
            arp->replies_in_window = 0;
        }
        if (arp->replies_in_window >= ARP_REPLY_MAX) {
            arp->stats.rate_limited++;
            return 0;
        }
        arp->replies_in_window++;
        u8 reply[ARP_PACKET_SIZE];
        build_packet(reply, 2, port->address, arp->local_address,
                     sender_hardware, sender_protocol);
        if (arp->transmit(sender_hardware, 0x0806, reply,
                          sizeof(reply), arp->transmit_context))
            return -1;
        arp->stats.replies_sent++;
        return 0;
    }
    arp->stats.replies_received++;
    if (target_protocol != arp->local_address ||
        !address_equal(target_hardware, port->address)) {
        arp->stats.malformed++;
        return -1;
    }
    learn(arp, sender_protocol, sender_hardware, arp->now);
    return 0;
}

int arp_lookup(struct arp_context *arp, u32 protocol_address,
               u8 hardware_address[6], u32 now) {
    if (!arp || !hardware_address || !protocol_valid(protocol_address))
        return -1;
    arp->now = now;
    struct arp_cache_entry *entry = find_entry(arp, protocol_address);
    if (!entry || entry->state != ARP_CACHE_REACHABLE) return -1;
    if ((i32)(now - entry->expires) >= 0) {
        entry->state = ARP_CACHE_EMPTY;
        arp->stats.cache_expired++;
        return -1;
    }
    for (u32 index = 0; index < 6; index++)
        hardware_address[index] = entry->hardware_address[index];
    return 0;
}

int arp_resolve(struct arp_context *arp, u32 protocol_address, u32 now) {
    if (!arp || !protocol_valid(protocol_address) ||
        protocol_address == arp->local_address)
        return -1;
    arp->now = now;
    struct arp_cache_entry *entry = find_entry(arp, protocol_address);
    if (entry && entry->state == ARP_CACHE_REACHABLE &&
        (i32)(now - entry->expires) < 0)
        return 0;
    if (!entry) entry = allocate_entry(arp, protocol_address, now);
    if (!entry) return -1;
    if ((i32)(now - entry->last_request) < (i32)arp->request_interval) {
        arp->stats.rate_limited++;
        return 1;
    }
    const u8 broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const u8 zero[6] = {0, 0, 0, 0, 0, 0};
    u8 request[ARP_PACKET_SIZE];
    build_packet(request, 1, arp->port->address, arp->local_address,
                 zero, protocol_address);
    if (arp->transmit(broadcast, 0x0806, request,
                      sizeof(request), arp->transmit_context))
        return -1;
    entry->last_request = now;
    entry->updated = now;
    entry->expires = now + arp->cache_ttl;
    entry->state = ARP_CACHE_PENDING;
    arp->stats.requests_sent++;
    return 1;
}

void arp_tick(struct arp_context *arp, u32 now) {
    if (!arp) return;
    arp->now = now;
    for (u32 index = 0; index < ARP_CACHE_MAX; index++) {
        struct arp_cache_entry *entry = &arp->cache[index];
        if (entry->state == ARP_CACHE_EMPTY ||
            (i32)(now - entry->expires) < 0)
            continue;
        entry->state = ARP_CACHE_EMPTY;
        entry->protocol_address = 0;
        arp->stats.cache_expired++;
    }
}

u32 arp_cache_count(const struct arp_context *arp, u32 state) {
    if (!arp) return 0;
    u32 count = 0;
    for (u32 index = 0; index < ARP_CACHE_MAX; index++)
        if (arp->cache[index].state == state) count++;
    return count;
}
