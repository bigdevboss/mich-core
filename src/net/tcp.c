#include "checksum.h"
#include "tcp.h"
#include "resource.h"

void *memcpy(void *dst, const void *src, usize_t length);

static u16 read16(const u8 *p) { return (u16)((u16)p[0] << 8) | p[1]; }
static u32 read32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
        ((u32)p[2] << 8) | p[3];
}
static void write16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void write32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

static int sequence_after(u32 left, u32 right);
static int sequence_before(u32 left, u32 right);

static u32 tcp_pseudo_sum4(u32 source, u32 destination, u32 length) {
    u8 pseudo[12];
    write32(pseudo, source);
    write32(pseudo + 4, destination);
    pseudo[8] = 0;
    pseudo[9] = 6;
    write16(pseudo + 10, (u16)length);
    return net_checksum_sum(0, pseudo, sizeof(pseudo));
}

static u32 tcp_pseudo_sum6(const u8 source[16], const u8 destination[16],
                           u32 length) {
    u32 sum = net_checksum_sum(0, source, 16);
    sum = net_checksum_sum(sum, destination, 16);
    u8 tail[8];
    tail[0] = (u8)(length >> 24);
    tail[1] = (u8)(length >> 16);
    tail[2] = (u8)(length >> 8);
    tail[3] = (u8)length;
    tail[4] = 0;
    tail[5] = 0;
    tail[6] = 0;
    tail[7] = 6;
    return net_checksum_sum(sum, tail, sizeof(tail));
}

u16 tcp_checksum_ipv4(u32 source, u32 destination,
                      const void *segment, u32 length) {
    if (!segment || length < TCP_HEADER_MIN || length > 0xFFFFu) return 0xFFFF;
    u32 sum = tcp_pseudo_sum4(source, destination, length);
    sum = net_checksum_sum(sum, segment, length);
    return net_checksum_finish(sum);
}

u16 tcp_checksum_ipv6(const u8 source[16], const u8 destination[16],
                      const void *segment, u32 length) {
    if (!source || !destination || !segment || length < TCP_HEADER_MIN)
        return 0xFFFF;
    u32 sum = tcp_pseudo_sum6(source, destination, length);
    sum = net_checksum_sum(sum, segment, length);
    return net_checksum_finish(sum);
}

int tcp_parse(const void *data, u32 length, struct tcp_segment_view *s) {
    if (!data || !s || length < TCP_HEADER_MIN) return -1;
    const u8 *p = data;
    u32 header = (u32)(p[12] >> 4) * 4;
    if (header < TCP_HEADER_MIN || header > length || header > 60) return -1;
    u16 source = read16(p), destination = read16(p + 2);
    u8 flags = p[13];
    if (!source || !destination || ((flags & TCP_FLAG_SYN) &&
        (flags & (TCP_FLAG_FIN | TCP_FLAG_RST)))) return -1;
    s->segment = p; s->payload = p + header; s->segment_length = length;
    s->header_length = header; s->payload_length = length - header;
    s->source_port = source; s->destination_port = destination;
    s->sequence = read32(p + 4); s->acknowledgement = read32(p + 8);
    s->flags = flags; s->window = read16(p + 14);
    s->urgent_pointer = read16(p + 18); s->mss = 0; s->window_scale = 0;
    s->sack_permitted = 0; s->timestamp_present = 0;
    s->timestamp_value = 0; s->timestamp_echo = 0;
    s->sack_block_count = 0;
    u32 offset = TCP_HEADER_MIN;
    u32 seen = 0;
    while (offset < header) {
        u8 kind = p[offset++];
        if (!kind) break;
        if (kind == 1) continue;
        if (offset >= header) return -1;
        u8 option_length = p[offset++];
        if (option_length < 2 ||
            (u32)(option_length - 2) > header - offset)
            return -1;
        if (kind == 2) {
            if (option_length != 4 || (seen & 1)) return -1;
            s->mss = read16(p + offset); if (!s->mss) return -1; seen |= 1;
        } else if (kind == 3) {
            if (option_length != 3 || (seen & 2) || p[offset] > 14) return -1;
            s->window_scale = p[offset]; seen |= 2;
        } else if (kind == 4) {
            if (option_length != 2 || (seen & 4)) return -1;
            s->sack_permitted = 1; seen |= 4;
        } else if (kind == 5) {
            if (option_length < 10 || (option_length - 2) % 8) return -1;
            u32 block_count = (option_length - 2) / 8;
            for (u32 block = 0; block < block_count; block++) {
                u32 left = read32(p + offset + block * 8);
                u32 right = read32(p + offset + block * 8 + 4);
                if (sequence_before(right, left)) continue;
                if (s->sack_block_count < 2) {
                    s->sack_blocks[s->sack_block_count][0] = left;
                    s->sack_blocks[s->sack_block_count][1] = right;
                    s->sack_block_count++;
                }
            }
        } else if (kind == 8) {
            if (option_length != 10 || (seen & 8)) return -1;
            s->timestamp_value = read32(p + offset);
            s->timestamp_echo = read32(p + offset + 4);
            s->timestamp_present = 1; seen |= 8;
        }
        offset += option_length - 2;
    }
    return 0;
}

int tcp_build_ipv4_opts(void *buffer, u32 capacity,
                        u32 source_address, u32 destination_address,
                        u16 source_port, u16 destination_port,
                        u32 sequence, u32 acknowledgement, u8 flags,
                        u16 window,
                        const void *options, u32 option_length,
                        const void *payload, u32 payload_length) {
    if (!buffer || option_length > TCP_OPTION_MAX || option_length % 4 ||
        capacity < TCP_HEADER_MIN + option_length ||
        !source_address || !destination_address ||
        !source_port || !destination_port ||
        (!payload && payload_length) ||
        payload_length > capacity - TCP_HEADER_MIN - option_length)
        return -1;
    u32 header_length = TCP_HEADER_MIN + option_length;
    u8 *p = buffer;
    for (u32 i = 0; i < header_length; i++) p[i] = 0;
    write16(p, source_port); write16(p + 2, destination_port);
    write32(p + 4, sequence); write32(p + 8, acknowledgement);
    p[12] = (u8)((header_length >> 2) << 4); p[13] = flags;
    write16(p + 14, window);
    if (option_length)
        memcpy(p + TCP_HEADER_MIN, options, option_length);
    u32 sum = tcp_pseudo_sum4(source_address, destination_address,
                              header_length + payload_length);
    if (payload_length)
        sum = net_checksum_copy(sum, p + header_length, payload,
                                payload_length);
    sum = net_checksum_sum(sum, p, header_length);
    write16(p + 16, net_checksum_finish(sum));
    return (int)(header_length + payload_length);
}

int tcp_build_ipv6_opts(void *buffer, u32 capacity,
                        const u8 source_address[16],
                        const u8 destination_address[16],
                        u16 source_port, u16 destination_port,
                        u32 sequence, u32 acknowledgement, u8 flags,
                        u16 window,
                        const void *options, u32 option_length,
                        const void *payload, u32 payload_length) {
    if (!buffer || !source_address || !destination_address ||
        option_length > TCP_OPTION_MAX || option_length % 4 ||
        capacity < TCP_HEADER_MIN + option_length ||
        !source_port || !destination_port ||
        (!payload && payload_length) ||
        payload_length > capacity - TCP_HEADER_MIN - option_length)
        return -1;
    u32 header_length = TCP_HEADER_MIN + option_length;
    u8 *p = buffer;
    for (u32 i = 0; i < header_length; i++) p[i] = 0;
    write16(p, source_port); write16(p + 2, destination_port);
    write32(p + 4, sequence); write32(p + 8, acknowledgement);
    p[12] = (u8)((header_length >> 2) << 4); p[13] = flags;
    write16(p + 14, window);
    if (option_length)
        memcpy(p + TCP_HEADER_MIN, options, option_length);
    u32 sum = tcp_pseudo_sum6(source_address, destination_address,
                              header_length + payload_length);
    if (payload_length)
        sum = net_checksum_copy(sum, p + header_length, payload,
                                payload_length);
    sum = net_checksum_sum(sum, p, header_length);
    write16(p + 16, net_checksum_finish(sum));
    return (int)(header_length + payload_length);
}

int tcp_build_ipv4(void *buffer, u32 capacity,
                   u32 source_address, u32 destination_address,
                   u16 source_port, u16 destination_port,
                   u32 sequence, u32 acknowledgement, u8 flags, u16 window,
                   const void *payload, u32 payload_length) {
    return tcp_build_ipv4_opts(buffer, capacity, source_address,
                               destination_address, source_port,
                               destination_port, sequence, acknowledgement,
                               flags, window, 0, 0, payload, payload_length);
}

int tcp_build_ipv6(void *buffer, u32 capacity,
                   const u8 source_address[16],
                   const u8 destination_address[16],
                   u16 source_port, u16 destination_port,
                   u32 sequence, u32 acknowledgement, u8 flags, u16 window,
                   const void *payload, u32 payload_length) {
    return tcp_build_ipv6_opts(buffer, capacity, source_address,
                               destination_address, source_port,
                               destination_port, sequence, acknowledgement,
                               flags, window, 0, 0, payload, payload_length);
}

static u64 sequence_mix(u64 value) {
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

void tcp_init(struct tcp_context *tcp, u64 seed) {
    if (!seed) seed = 1;
    tcp->sequence_key0 = sequence_mix(seed ^ 0x736F6D6570736575ULL);
    tcp->sequence_key1 = sequence_mix(seed ^ 0x646F72616E646F6DULL);
    tcp->next_sequence = 1;
    tcp->now = 0;
    for (u32 i = 0; i < TCP_CONNECTION_MAX; i++) {
        tcp->connections[i].generation = i + 1;
        tcp->connections[i].active = 0; tcp->connections[i].state = TCP_STATE_CLOSED;
    }
    u8 *p = (u8 *)&tcp->stats;
    for (usize_t i = 0; i < sizeof(tcp->stats); i++) p[i] = 0;
    tcp->cc = tcp_cc_bbr();
    tcp->mss = TCP_RETRANSMIT_DATA_MAX;
    tcp->ack_filter = 0;
    tcp->timer_epoch = 1;
    tcp->deadline_count = 0;
    tcp->pmtu_blackhole = 0;
    tcp->pmtu_blackhole_context = 0;
}

static u32 next_initial_sequence(struct tcp_context *tcp,
                                 const struct tcp_connection *connection) {
    u64 tuple = ((u64)connection->family << 56) |
        ((u64)connection->local_port << 16) | connection->remote_port;
    tuple ^= ((u64)connection->local_address << 32) |
        connection->remote_address;
    if (connection->family == 6) {
        for (u32 byte = 0; byte < 16; byte++) {
            tuple ^= (u64)connection->local_address6[byte] << ((byte & 7) * 8);
            tuple = sequence_mix(tuple);
            tuple ^= (u64)connection->remote_address6[byte] << ((byte & 7) * 8);
        }
    }
    u64 counter = tcp->next_sequence++;
    u64 value = sequence_mix(tuple ^ tcp->sequence_key0 ^
                             (counter * 0x9E3779B97F4A7C15ULL));
    value = sequence_mix(value ^ tcp->sequence_key1);
    return (u32)(value ^ (value >> 32));
}

static u32 loan_spanned(const struct tcp_send_loan *loan) {
    return (loan->offset + loan->length + 4095) / 4096 -
        loan->offset / 4096;
}

static const u8 *loan_bytes(const struct tcp_send_loan *loan, u32 position) {
    struct page_resource *resource = page_resource_get(loan->pages);
    if (!resource) return 0;
    u32 absolute = loan->offset + position;
    if (absolute / 4096 >= resource->pages) return 0;
    return (const u8 *)(uptr_t)resource->physical[absolute / 4096] +
        absolute % 4096;
}

static void loan_release(struct tcp_connection *c,
                         struct tcp_send_loan *loan) {
    page_resource_unpin(loan->pages);
    c->send_loan_pages -= loan_spanned(loan);
    loan->pages = 0;
    loan->offset = 0;
    loan->length = 0;
    loan->position = 0;
    c->send_loan_head = (c->send_loan_head + 1) % TCP_SEND_LOAN_MAX;
    c->send_loan_count--;
}

static void release_loans(struct tcp_connection *c) {
    for (u32 index = 0; index < TCP_SEND_LOAN_MAX; index++) {
        struct tcp_send_loan *loan = &c->send_loans[index];
        if (!loan->pages) continue;
        page_resource_unpin(loan->pages);
        loan->pages = 0;
        loan->offset = 0;
        loan->length = 0;
        loan->position = 0;
    }
    c->send_loan_head = 0;
    c->send_loan_count = 0;
    c->send_loan_pages = 0;
}

static u32 grant_spanned(const struct tcp_receive_grant *grant) {
    return (grant->offset + grant->length + 4095) / 4096 -
        grant->offset / 4096;
}

static u8 *grant_bytes(struct tcp_receive_grant *grant, u32 position) {
    struct page_resource *resource = page_resource_get(grant->pages);
    if (!resource) return 0;
    u32 absolute = grant->offset + position;
    if (absolute / 4096 >= resource->pages) return 0;
    return (u8 *)(uptr_t)resource->physical[absolute / 4096] +
        absolute % 4096;
}

static void grant_release(struct tcp_connection *c,
                          struct tcp_receive_grant *grant) {
    page_resource_unpin(grant->pages);
    c->receive_grant_pages -= grant_spanned(grant);
    grant->pages = 0;
    grant->offset = 0;
    grant->length = 0;
    grant->position = 0;
    c->receive_grant_head =
        (c->receive_grant_head + 1) % TCP_RECEIVE_GRANT_MAX;
    c->receive_grant_count--;
}

static void release_grants(struct tcp_connection *c) {
    for (u32 index = 0; index < TCP_RECEIVE_GRANT_MAX; index++) {
        struct tcp_receive_grant *grant = &c->receive_grants[index];
        if (!grant->pages) continue;
        page_resource_unpin(grant->pages);
        grant->pages = 0;
        grant->offset = 0;
        grant->length = 0;
        grant->position = 0;
    }
    c->receive_grant_head = 0;
    c->receive_grant_count = 0;
    c->receive_grant_pages = 0;
}

static void refresh_receive_window(struct tcp_connection *c) {
    u32 space = TCP_RECEIVE_BUFFER_MAX - c->receive_buffer_length;
    for (u32 index = 0; index < TCP_RECEIVE_GRANT_MAX; index++) {
        const struct tcp_receive_grant *grant = &c->receive_grants[index];
        if (grant->pages) space += grant->length - grant->position;
    }
    c->receive_window = space > 0xFFFFu ? (u16)0xFFFF : (u16)space;
}

static u32 grant_write(struct tcp_receive_grant *grant,
                       const u8 *data, u32 length) {
    u32 done = 0;
    while (done < length) {
        u8 *target = grant_bytes(grant, grant->position + done);
        if (!target) return done;
        u32 within =
            4096 - (grant->offset + grant->position + done) % 4096;
        u32 chunk = length - done;
        if (chunk > within) chunk = within;
        for (u32 index = 0; index < chunk; index++)
            target[index] = data[done + index];
        done += chunk;
    }
    return done;
}

static struct tcp_connection *allocate(struct tcp_context *tcp, u32 *slot) {
    for (u32 i = 0; i < TCP_CONNECTION_MAX; i++) {
        struct tcp_connection *c = &tcp->connections[i];
        if (c->active) continue;
        *slot = i;
        release_loans(c);
        release_grants(c);
        c->active = 1;
        c->family = 0;
        c->local_address = 0;
        c->remote_address = 0;
        for (u32 byte = 0; byte < 16; byte++) {
            c->local_address6[byte] = 0;
            c->remote_address6[byte] = 0;
        }
        c->send_unacknowledged = 0;
        c->send_next = 0;
        c->receive_next = 0;
        c->local_port = 0;
        c->remote_port = 0;
        c->receive_window = TCP_RECEIVE_BUFFER_MAX;
        c->send_window = 65535;
        c->remote_mss = TCP_RETRANSMIT_DATA_MAX;
        c->congestion_window = TCP_RETRANSMIT_DATA_MAX * 2;
        c->slow_start_threshold = 65535;
        c->retransmission_timeout = TCP_RTO_INITIAL;
        c->smoothed_rtt = 0;
        c->rtt_variance = 0;
        c->duplicate_acks = 0;
        c->persist_deadline = 0;
        c->persist_interval = TCP_RTO_INITIAL;
        c->time_wait_deadline = 0;
        c->error = 0;
        c->eof = 0;
        c->send_fin = 0;
        c->parent_listener = 0;
        c->listen_backlog = 0;
        c->passive = 0;
        c->accepted = 0;
        c->detached = 0;
        c->send_buffer_length = 0;
        c->send_buffer_offset = 0;
        c->receive_buffer_length = 0;
        c->receive_grant_bytes = 0;
        c->timer_version = tcp->timer_epoch++;
        c->peer_sack = 0;
        c->ack_filter_until = 0;
        c->ack_pending = 0;
        c->pmtu_blackhole_rtos = 0;
        for (u32 entry = 0; entry < TCP_RETRANSMISSION_MAX; entry++)
            c->retransmissions[entry].active = 0;
        for (u32 entry = 0; entry < TCP_OUT_OF_ORDER_MAX; entry++)
            c->out_of_order[entry].active = 0;
        tcp->cc->init(c);
        c->state = TCP_STATE_CLOSED;
        return c;
    }
    return 0;
}
static u64 id_for(struct tcp_connection *c, u32 slot) { return ((u64)c->generation << 32) | (slot + 1); }
static struct tcp_connection *by_id(struct tcp_context *tcp, u64 id) {
    u32 slot = (u32)id;
    if (!slot || slot > TCP_CONNECTION_MAX) return 0;
    struct tcp_connection *c = &tcp->connections[slot - 1];
    return c->active && c->generation == (u32)(id >> 32) ? c : 0;
}

//
// Global deadline min-heap. Every timer (retransmit entries, persist
// probes, TIME_WAIT expiry, ACK-filter flushes) is a node; tcp_tick pops
// the minimum. Deadlines are updated by pushing a new node: stale nodes
// fail their per-kind validation at pop time (the entry's current
// deadline no longer matches), and connection reuse is caught by the
// timer_version token. This replaces the per-tick O(connections * slots)
// linear sweep with O(log N) per event and an O(1) idle tick.
//
static void deadline_push(struct tcp_context *tcp, u32 slot, u32 kind,
                          u32 entry, u32 deadline) {
    if (tcp->deadline_count >= TCP_DEADLINE_MAX) return;
    struct tcp_deadline *heap = tcp->deadlines;
    u32 i = tcp->deadline_count++;
    heap[i].deadline = deadline;
    heap[i].slot = slot;
    heap[i].kind = kind;
    heap[i].entry = entry;
    heap[i].version = tcp->connections[slot].timer_version;
    while (i) {
        u32 parent = (i - 1) / 2;
        if (heap[parent].deadline <= heap[i].deadline) break;
        struct tcp_deadline tmp = heap[parent];
        heap[parent] = heap[i];
        heap[i] = tmp;
        i = parent;
    }
}

static void deadline_pop(struct tcp_context *tcp) {
    u32 last = --tcp->deadline_count;
    if (!last) return;
    struct tcp_deadline item = tcp->deadlines[last];
    u32 i = 0;
    tcp->deadlines[i] = item;
    for (;;) {
        u32 left = i * 2 + 1;
        u32 right = left + 1;
        u32 smallest = i;
        if (left < last &&
            tcp->deadlines[left].deadline < tcp->deadlines[smallest].deadline)
            smallest = left;
        if (right < last &&
            tcp->deadlines[right].deadline < tcp->deadlines[smallest].deadline)
            smallest = right;
        if (smallest == i) break;
        tcp->deadlines[i] = tcp->deadlines[smallest];
        i = smallest;
    }
    tcp->deadlines[i] = item;
}

u64 tcp_listen(struct tcp_context *tcp, u32 address, u16 port) {
    if (!tcp || !address || !port) return 0;
    for (u32 i = 0; i < TCP_CONNECTION_MAX; i++)
        if (tcp->connections[i].active && tcp->connections[i].family == 4 &&
            tcp->connections[i].local_port == port)
            return 0;
    u32 slot; struct tcp_connection *c = allocate(tcp, &slot); if (!c) return 0;
    c->family = 4; c->local_address = address; c->remote_address = 0; c->local_port = port;
    c->remote_port = 0; c->state = TCP_STATE_LISTEN; c->receive_window = TCP_RECEIVE_BUFFER_MAX;
    c->listen_backlog = 16;
    return id_for(c, slot);
}

u64 tcp_listen_ipv6(struct tcp_context *tcp,
                    const u8 address[16], u16 port) {
    if (!tcp || !address || !port || address[0] == 0xFF) return 0;
    u32 nonzero = 0;
    for (u32 byte = 0; byte < 16; byte++) nonzero |= address[byte];
    if (!nonzero) return 0;
    for (u32 index = 0; index < TCP_CONNECTION_MAX; index++) {
        struct tcp_connection *existing = &tcp->connections[index];
        if (!existing->active || existing->family != 6 ||
            existing->local_port != port)
            continue;
        int equal = 1;
        for (u32 byte = 0; byte < 16; byte++)
            if (existing->local_address6[byte] != address[byte]) equal = 0;
        if (equal) return 0;
    }
    u32 slot;
    struct tcp_connection *c = allocate(tcp, &slot);
    if (!c) return 0;
    c->family = 6;
    for (u32 byte = 0; byte < 16; byte++)
        c->local_address6[byte] = address[byte];
    c->local_port = port;
    c->state = TCP_STATE_LISTEN;
    c->receive_window = TCP_RECEIVE_BUFFER_MAX;
    c->listen_backlog = 16;
    return id_for(c, slot);
}

int tcp_set_listener_backlog(struct tcp_context *tcp, u64 listener_id,
                             u32 backlog) {
    struct tcp_connection *listener = by_id(tcp, listener_id);
    if (!listener || listener->state != TCP_STATE_LISTEN ||
        !backlog || backlog >= TCP_CONNECTION_MAX)
        return -1;
    listener->listen_backlog = backlog;
    return 0;
}

u64 tcp_active_open(struct tcp_context *tcp, u32 local, u16 lport, u32 remote, u16 rport) {
    if (!tcp || !local || !lport || !remote || !rport) return 0;
    for (u32 index = 0; index < TCP_CONNECTION_MAX; index++) {
        struct tcp_connection *existing = &tcp->connections[index];
        if (existing->active && existing->family == 4 &&
            existing->local_address == local &&
            existing->remote_address == remote &&
            existing->local_port == lport && existing->remote_port == rport)
            return 0;
    }
    u32 slot; struct tcp_connection *c = allocate(tcp, &slot); if (!c) return 0;
    c->family = 4; c->local_address = local; c->remote_address = remote; c->local_port = lport; c->remote_port = rport;
    c->send_unacknowledged = next_initial_sequence(tcp, c);
    c->send_next = c->send_unacknowledged + 1;
    c->receive_next = 0; c->receive_window = TCP_RECEIVE_BUFFER_MAX;
    c->state = TCP_STATE_SYN_SENT; tcp->stats.connections_opened++;
    return id_for(c, slot);
}

static int address6_equal(const u8 left[16], const u8 right[16]);

u64 tcp_active_open_ipv6(struct tcp_context *tcp,
                         const u8 local[16], u16 lport,
                         const u8 remote[16], u16 rport) {
    if (!tcp || !local || !remote || !lport || !rport ||
        local[0] == 0xFF || remote[0] == 0xFF)
        return 0;
    u32 nonzero_local = 0;
    u32 nonzero_remote = 0;
    for (u32 byte = 0; byte < 16; byte++) {
        nonzero_local |= local[byte];
        nonzero_remote |= remote[byte];
    }
    if (!nonzero_local || !nonzero_remote) return 0;
    for (u32 index = 0; index < TCP_CONNECTION_MAX; index++) {
        struct tcp_connection *existing = &tcp->connections[index];
        if (existing->active && existing->family == 6 &&
            existing->local_port == lport && existing->remote_port == rport &&
            address6_equal(existing->local_address6, local) &&
            address6_equal(existing->remote_address6, remote))
            return 0;
    }
    u32 slot;
    struct tcp_connection *c = allocate(tcp, &slot);
    if (!c) return 0;
    c->family = 6;
    for (u32 byte = 0; byte < 16; byte++) {
        c->local_address6[byte] = local[byte];
        c->remote_address6[byte] = remote[byte];
    }
    c->local_port = lport;
    c->remote_port = rport;
    c->send_unacknowledged = next_initial_sequence(tcp, c);
    c->send_next = c->send_unacknowledged + 1;
    c->receive_next = 0;
    c->receive_window = TCP_RECEIVE_BUFFER_MAX;
    c->state = TCP_STATE_SYN_SENT;
    tcp->stats.connections_opened++;
    return id_for(c, slot);
}

int tcp_accept_pending(struct tcp_context *tcp, u64 listener_id) {
    struct tcp_connection *listener = by_id(tcp, listener_id);
    if (!listener || listener->state != TCP_STATE_LISTEN) return -1;
    for (u32 index = 0; index < TCP_CONNECTION_MAX; index++) {
        struct tcp_connection *connection = &tcp->connections[index];
        if (connection->active && connection->passive &&
            !connection->accepted &&
            connection->parent_listener == listener_id &&
            connection->state == TCP_STATE_ESTABLISHED)
            return 1;
    }
    return 0;
}

int tcp_accept(struct tcp_context *tcp, u64 listener_id,
               u64 *connection_id) {
    struct tcp_connection *listener = by_id(tcp, listener_id);
    if (!listener || listener->state != TCP_STATE_LISTEN || !connection_id)
        return -1;
    for (u32 index = 0; index < TCP_CONNECTION_MAX; index++) {
        struct tcp_connection *connection = &tcp->connections[index];
        if (!connection->active || !connection->passive ||
            connection->accepted ||
            connection->parent_listener != listener_id ||
            connection->state != TCP_STATE_ESTABLISHED)
            continue;
        connection->accepted = 1;
        connection->parent_listener = 0;
        *connection_id = id_for(connection, index);
        return 0;
    }
    return 1;
}

static void response(struct tcp_response *r, struct tcp_connection *c, u8 flags) {
    r->source_port = c->local_port; r->destination_port = c->remote_port;
    r->sequence = c->send_next; r->acknowledgement = c->receive_next;
    r->window = c->receive_window; r->flags = flags; r->valid = 1;
    r->option_length = 0;
    r->sack_length = 0;
}

// Advertise only what the retransmit and OOO slots can hold.
static u16 local_mss(const struct tcp_context *tcp) {
    u32 mss = tcp && tcp->mss ? tcp->mss : TCP_RETRANSMIT_DATA_MAX;
    if (mss > TCP_RETRANSMIT_DATA_MAX) mss = TCP_RETRANSMIT_DATA_MAX;
    if (mss < 46) mss = 46;
    return (u16)mss;
}

// MSS + NOP + NOP + SACK-permitted: 8 option bytes, 4-byte aligned.
static void syn_options(struct tcp_context *tcp, u8 *options, u16 *length) {
    u16 mss = local_mss(tcp);
    options[0] = 2; options[1] = 4;
    options[2] = (u8)(mss >> 8); options[3] = (u8)mss;
    options[4] = 1; options[5] = 1;
    options[6] = 4; options[7] = 2;
    *length = 8;
}

//
// Derive SACK blocks from the out-of-order buffer. The OOO queue is the
// single source of reassembly state, so it doubles as the SACK range
// table: no separate range storage. Up to two blocks (the highest two,
// RFC 2018 fits two in a 40-byte header) after sorting and coalescing.
//
static u16 write_sack(struct tcp_context *tcp, struct tcp_connection *c,
                      u8 *out, u32 cap) {
    u32 seqs[TCP_OUT_OF_ORDER_MAX];
    u32 ends[TCP_OUT_OF_ORDER_MAX];
    u32 count = 0;
    for (u32 index = 0; index < TCP_OUT_OF_ORDER_MAX; index++) {
        struct tcp_out_of_order *e = &c->out_of_order[index];
        if (!e->active) continue;
        u32 end = e->sequence + e->length +
            ((e->flags & TCP_FLAG_FIN) != 0 ? 1 : 0);
        if (sequence_before(end, c->receive_next)) continue;
        seqs[count] = e->sequence;
        ends[count] = end;
        count++;
    }
    if (!count) return 0;
    for (u32 i = 1; i < count; i++) {
        u32 sq = seqs[i];
        u32 en = ends[i];
        u32 j = i;
        while (j && seqs[j - 1] > sq) {
            seqs[j] = seqs[j - 1];
            ends[j] = ends[j - 1];
            j--;
        }
        seqs[j] = sq;
        ends[j] = en;
    }
    u32 bleft[2];
    u32 bright[2];
    u32 blocks = 0;
    for (u32 i = 0; i < count; i++) {
        u32 left = seqs[i];
        u32 right = ends[i];
        if (blocks && (i32)(left - bright[blocks - 1]) <= 0) {
            if (right > bright[blocks - 1]) bright[blocks - 1] = right;
            continue;
        }
        if (blocks < 2) {
            bleft[blocks] = left;
            bright[blocks] = right;
            blocks++;
        } else {
            bleft[0] = bleft[1];
            bright[0] = bright[1];
            bleft[1] = left;
            bright[1] = right;
        }
    }
    if (!blocks) return 0;
    u16 length = (u16)(4 + 8 * blocks);
    if (length > cap) return 0;
    out[0] = 1; out[1] = 1; out[2] = 5; out[3] = (u8)(2 + 8 * blocks);
    for (u32 b = 0; b < blocks; b++) {
        write32(out + 4 + b * 8, bleft[b]);
        write32(out + 4 + b * 8 + 4, bright[b]);
    }
    tcp->stats.sack_blocks_sent += blocks;
    return length;
}

static void fill_sack_blocks(struct tcp_context *tcp, struct tcp_response *r,
                             struct tcp_connection *c) {
    r->sack_length = write_sack(tcp, c, r->sack, sizeof(r->sack));
}

static int sequence_after(u32 left, u32 right) {
    return (i32)(left - right) > 0;
}

static int sequence_before(u32 left, u32 right) {
    return (i32)(left - right) < 0;
}

static void update_rtt(struct tcp_connection *c, u32 sample) {
    if (!sample) sample = 1;
    if (!c->smoothed_rtt) {
        c->smoothed_rtt = sample;
        c->rtt_variance = sample / 2;
    } else {
        u32 difference = c->smoothed_rtt > sample ?
            c->smoothed_rtt - sample : sample - c->smoothed_rtt;
        c->rtt_variance = (c->rtt_variance * 3 + difference) / 4;
        c->smoothed_rtt = (c->smoothed_rtt * 7 + sample) / 8;
    }
    u64 rto64 = (u64)c->smoothed_rtt + (u64)c->rtt_variance * 4;
    u32 rto = rto64 > TCP_RTO_MAX ? TCP_RTO_MAX : (u32)rto64;
    if (rto < 20) rto = 20;
    c->retransmission_timeout = rto;
}

static void rt_sample(struct tcp_context *tcp, struct tcp_connection *c,
                      u32 sample) {
    update_rtt(c, sample);
    u32 flight = c->send_next - c->send_unacknowledged;
    tcp->cc->on_rtt(c, sample, flight, tcp->now);
}

//
// ACK processing. SACK blocks retire fully covered in-flight entries
// (releasing their retransmit copies); the lowest still-active entry is
// then the loss candidate for fast retransmit. Congestion-window policy
// is delegated to the pluggable CC; entry deadline changes are mirrored
// into the global deadline heap.
//
static void acknowledge(struct tcp_context *tcp, struct tcp_connection *c,
                        u32 slot, const struct tcp_segment_view *s) {
    if (s->sack_block_count) {
        u32 retired = 0;
        for (u32 block = 0; block < s->sack_block_count; block++) {
            u32 left = s->sack_blocks[block][0];
            u32 right = s->sack_blocks[block][1];
            for (u32 index = 0; index < TCP_RETRANSMISSION_MAX; index++) {
                struct tcp_retransmission *e = &c->retransmissions[index];
                if (!e->active) continue;
                if (sequence_before(left, e->sequence)) continue;
                if (sequence_after(right, e->end_sequence)) continue;
                e->active = 0;
                retired++;
                c->bbr_delivered += e->length;
            }
        }
        if (retired) {
            tcp->stats.sack_retired_segments += retired;
            tcp->stats.sack_blocks_received += s->sack_block_count;
        }
    }
    if (sequence_after(s->acknowledgement, c->send_unacknowledged) &&
        !sequence_after(s->acknowledgement, c->send_next)) {
        u32 advance = s->acknowledgement - c->send_unacknowledged;
        c->send_unacknowledged = s->acknowledgement;
        c->duplicate_acks = 0;
        c->pmtu_blackhole_rtos = 0;
        for (u32 index = 0; index < TCP_RETRANSMISSION_MAX; index++) {
            struct tcp_retransmission *entry = &c->retransmissions[index];
            if (!entry->active ||
                sequence_after(entry->end_sequence, s->acknowledgement))
                continue;
            if (!entry->retransmitted && tcp->now >= entry->sent_at)
                rt_sample(tcp, c, tcp->now - entry->sent_at);
            entry->active = 0;
        }
        u32 flight = c->send_next - c->send_unacknowledged;
        tcp->cc->on_ack(c, advance, flight, tcp->now);
    } else if (s->acknowledgement == c->send_unacknowledged) {
        c->duplicate_acks++;
        tcp->stats.duplicate_acknowledgements++;
        if (c->duplicate_acks == 3) {
            tcp->stats.fast_retransmits++;
            if (s->sack_block_count)
                tcp->stats.sack_fast_retransmits++;
            tcp->cc->on_fast_retransmit(c, c->duplicate_acks);
            tcp->stats.cc_recovery_events++;
            for (u32 index = 0; index < TCP_RETRANSMISSION_MAX; index++) {
                struct tcp_retransmission *entry =
                    &c->retransmissions[index];
                if (!entry->active) continue;
                entry->fast_retransmit = 1;
                entry->deadline = tcp->now;
                deadline_push(tcp, slot, TCP_TIMER_RETRANSMIT,
                              index, entry->deadline);
                break;
            }
        }
    }
}

static int append_receive(struct tcp_context *tcp,
                          struct tcp_connection *c,
                          const u8 *data, u32 length) {
    // Grants take the head of the byte stream; only the spill beyond the
    // grant queue reaches the receive buffer, so grant bytes are always
    // older than buffer bytes and the read order stays well defined.
    u32 grant_space = 0;
    for (u32 index = 0; index < TCP_RECEIVE_GRANT_MAX; index++) {
        const struct tcp_receive_grant *grant = &c->receive_grants[index];
        if (grant->pages) grant_space += grant->length - grant->position;
    }
    if (length > grant_space +
        TCP_RECEIVE_BUFFER_MAX - c->receive_buffer_length)
        return -1;
    u32 granted = 0;
    while (granted < length && c->receive_grant_count) {
        struct tcp_receive_grant *grant =
            &c->receive_grants[c->receive_grant_head];
        u32 room = grant->length - grant->position;
        u32 chunk = length - granted < room ? length - granted : room;
        if (grant_write(grant, data + granted, chunk) != chunk)
            return -1;
        grant->position += chunk;
        granted += chunk;
        if (grant->position == grant->length)
            grant_release(c, grant);
    }
    if (granted < length) {
        u32 spill = length - granted;
        memcpy(c->receive_buffer + c->receive_buffer_length,
               data + granted, spill);
        c->receive_buffer_length += spill;
    }
    c->receive_grant_bytes += granted;
    refresh_receive_window(c);
    tcp->stats.bytes_received += length;
    return 0;
}

static void merge_out_of_order(struct tcp_context *tcp,
                               struct tcp_connection *c) {
    int progress;
    do {
        progress = 0;
        for (u32 index = 0; index < TCP_OUT_OF_ORDER_MAX; index++) {
            struct tcp_out_of_order *entry = &c->out_of_order[index];
            if (!entry->active || entry->sequence != c->receive_next) continue;
            if (append_receive(tcp, c, entry->data, entry->length)) return;
            c->receive_next += entry->length;
            if (entry->flags & TCP_FLAG_FIN) c->receive_next++;
            entry->active = 0;
            progress = 1;
        }
    } while (progress);
}

// Returns 1 when queued, 0 when the segment was already queued (a
// retransmission of SACKed data), -1 when dropped or full.
static int queue_out_of_order(struct tcp_context *tcp,
                              struct tcp_connection *c,
                              const struct tcp_segment_view *segment) {
    if (segment->payload_length > TCP_RETRANSMIT_DATA_MAX) {
        tcp->stats.out_of_order_dropped++;
        return -1;
    }
    for (u32 index = 0; index < TCP_OUT_OF_ORDER_MAX; index++) {
        struct tcp_out_of_order *entry = &c->out_of_order[index];
        if (entry->active && entry->sequence == segment->sequence) return 0;
        if (entry->active) continue;
        entry->sequence = segment->sequence;
        entry->length = (u16)segment->payload_length;
        entry->flags = segment->flags;
        if (segment->payload_length)
            memcpy(entry->data, segment->payload, segment->payload_length);
        entry->active = 1;
        tcp->stats.out_of_order_queued++;
        return 1;
    }
    tcp->stats.out_of_order_dropped++;
    return -1;
}

//
// ACK-filter (FreeBSD tcp_ackfilter, mode 1): after acknowledging new
// in-order data, suppress further pure-ACK responses until one RTT
// elapses; a deadline-heap event flushes the pending ACK. OOO, duplicate
// and FIN responses always bypass the filter so fast retransmit
// signalling and SACK never lose a segment of latency.
//
static void data_ack(struct tcp_context *tcp, struct tcp_response *r,
                     struct tcp_connection *c, u32 slot, u32 flags) {
    if (tcp->ack_filter && !(flags & TCP_FLAG_FIN) &&
        c->ack_filter_until && tcp->now < c->ack_filter_until) {
        c->ack_pending = 1;
        tcp->stats.acks_filtered++;
        return;
    }
    response(r, c, TCP_FLAG_ACK);
    if (c->peer_sack) fill_sack_blocks(tcp, r, c);
    tcp->stats.acks_sent++;
    if (tcp->ack_filter && !(flags & TCP_FLAG_FIN)) {
        u32 interval = c->smoothed_rtt ?
            c->smoothed_rtt : TCP_ACK_FILTER_INTERVAL_MIN;
        c->ack_filter_until = tcp->now + interval;
        deadline_push(tcp, slot, TCP_TIMER_ACK_FILTER, 0,
                      c->ack_filter_until);
    }
}

static int track_retransmission(struct tcp_context *tcp,
                                struct tcp_connection *c,
                                u32 slot, u32 sequence,
                                u8 flags, const u8 *data, u32 length,
                                u32 now) {
    if (length > TCP_RETRANSMIT_DATA_MAX) return -1;
    for (u32 index = 0; index < TCP_RETRANSMISSION_MAX; index++) {
        struct tcp_retransmission *entry = &c->retransmissions[index];
        if (entry->active) continue;
        entry->sequence = sequence;
        entry->end_sequence = sequence + length +
            ((flags & TCP_FLAG_SYN) != 0) + ((flags & TCP_FLAG_FIN) != 0);
        entry->deadline = now + c->retransmission_timeout;
        entry->sent_at = now;
        entry->length = (u16)length;
        entry->flags = flags;
        entry->retries = 0;
        entry->retransmitted = 0;
        entry->fast_retransmit = 0;
        if (length)
            memcpy(entry->data, data, length);
        entry->active = 1;
        deadline_push(tcp, slot, TCP_TIMER_RETRANSMIT, index, entry->deadline);
        return 1;
    }
    return 0;
}

static void enter_time_wait(struct tcp_context *tcp, struct tcp_connection *c,
                            u32 slot) {
    c->state = TCP_STATE_TIME_WAIT;
    c->time_wait_deadline = tcp->now + TCP_TIME_WAIT_TICKS;
    deadline_push(tcp, slot, TCP_TIMER_TIME_WAIT, 0, c->time_wait_deadline);
}

// 0 = processed, 2 = in-order FIN consumed, -1 = hard fail.
static int input_payload(struct tcp_context *tcp, struct tcp_connection *c,
                         u32 slot, const struct tcp_segment_view *s,
                         struct tcp_response *r) {
    if (!(s->flags & TCP_FLAG_ACK) || (s->flags & TCP_FLAG_SYN)) {
        tcp->stats.sequence_errors++;
        response(r, c, TCP_FLAG_ACK);
        return -1;
    }
    if (sequence_after(s->sequence, c->receive_next)) {
        int queued = queue_out_of_order(tcp, c, s);
        if (queued == 0) tcp->stats.sack_duplicate_acks++;
        tcp->stats.sequence_errors++;
        response(r, c, TCP_FLAG_ACK);
        if (c->peer_sack) fill_sack_blocks(tcp, r, c);
        return 0;
    }
    if (sequence_before(s->sequence, c->receive_next)) {
        tcp->stats.sequence_errors++;
        response(r, c, TCP_FLAG_ACK);
        if (c->peer_sack) fill_sack_blocks(tcp, r, c);
        return 0;
    }
    if (append_receive(tcp, c, s->payload, s->payload_length)) {
        response(r, c, TCP_FLAG_ACK);
        return -1;
    }
    c->receive_next += s->payload_length;
    int fin = (s->flags & TCP_FLAG_FIN) != 0;
    if (fin) {
        c->receive_next++;
        c->eof = 1;
    }
    merge_out_of_order(tcp, c);
    if (s->payload_length || fin)
        data_ack(tcp, r, c, slot, s->flags);
    return fin ? 2 : 0;
}

static int our_fin_acked(const struct tcp_connection *c,
                         const struct tcp_segment_view *s) {
    return (s->flags & TCP_FLAG_ACK) && s->acknowledgement == c->send_next;
}

// 1 = this packet was consumed as a closing-state event.
static int input_closing(struct tcp_context *tcp, struct tcp_connection *c,
                         u32 slot, const struct tcp_segment_view *s,
                         struct tcp_response *r) {
    if (c->state == TCP_STATE_CLOSE_WAIT) return 1;
    if (c->state == TCP_STATE_TIME_WAIT) {
        if (s->flags & TCP_FLAG_FIN) response(r, c, TCP_FLAG_ACK);
        return 1;
    }
    if (c->state == TCP_STATE_LAST_ACK) {
        if (our_fin_acked(c, s)) {
            c->active = 0;
            c->state = TCP_STATE_CLOSED;
            c->generation++;
            if (!c->generation) c->generation = 1;
            c->timer_version = tcp->timer_epoch++;
            tcp->stats.connections_closed++;
        }
        return 1;
    }
    if (c->state == TCP_STATE_CLOSING) {
        if (our_fin_acked(c, s)) enter_time_wait(tcp, c, slot);
        return 1;
    }
    if (c->state != TCP_STATE_FIN_WAIT_1 &&
        c->state != TCP_STATE_FIN_WAIT_2)
        return 0;
    if (c->state == TCP_STATE_FIN_WAIT_1 && our_fin_acked(c, s))
        c->state = TCP_STATE_FIN_WAIT_2;
    int result = input_payload(tcp, c, slot, s, r);
    if (result < 0) return 1;
    if (result != 2) return 1;
    if (c->state == TCP_STATE_FIN_WAIT_2)
        enter_time_wait(tcp, c, slot);
    else
        c->state = TCP_STATE_CLOSING;
    return 1;
}

// One state machine for a matched connection. Address lookup stays
// in the IPv4 and IPv6 receivers.
static int tcp_input(struct tcp_context *tcp, struct tcp_connection *c,
                     const struct tcp_segment_view *s,
                     struct tcp_response *r) {
    u32 slot = (u32)(c - tcp->connections);
    r->connection_id = id_for(c, slot);
    if (!(s->flags & TCP_FLAG_RST)) {
        c->send_window = s->window;
        if ((s->flags & TCP_FLAG_SYN) && s->mss)
            c->remote_mss = s->mss < TCP_RETRANSMIT_DATA_MAX ?
                s->mss : TCP_RETRANSMIT_DATA_MAX;
        if (s->flags & TCP_FLAG_SYN)
            c->peer_sack = s->sack_permitted;
    }
    if (s->flags & TCP_FLAG_RST) {
        int acceptable = c->state == TCP_STATE_SYN_SENT ?
            ((s->flags & TCP_FLAG_ACK) &&
             s->acknowledgement == c->send_next) :
            s->sequence == c->receive_next;
        if (!acceptable) {
            tcp->stats.sequence_errors++;
            if (c->state == TCP_STATE_ESTABLISHED)
                response(r, c, TCP_FLAG_ACK);
            return -1;
        }
        c->state = TCP_STATE_CLOSED;
        c->error = -104;
        c->eof = 1;
        if (c->detached) {
            c->active = 0;
            c->generation++;
            if (!c->generation) c->generation = 1;
            c->timer_version = tcp->timer_epoch++;
        }
        tcp->stats.reset_received++;
        tcp->stats.connections_closed++;
        return 0;
    }
    if (s->flags & TCP_FLAG_ACK) acknowledge(tcp, c, slot, s);
    if (c->state == TCP_STATE_SYN_SENT &&
        (s->flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) ==
            (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
        s->acknowledgement == c->send_next) {
        c->receive_next = s->sequence + 1;
        c->send_unacknowledged = s->acknowledgement;
        c->state = TCP_STATE_ESTABLISHED;
        response(r, c, TCP_FLAG_ACK);
        return 0;
    }
    if (c->state == TCP_STATE_SYN_RECEIVED &&
        (s->flags & TCP_FLAG_ACK) && s->acknowledgement == c->send_next) {
        c->send_unacknowledged = s->acknowledgement;
        c->state = TCP_STATE_ESTABLISHED;
        return 0;
    }
    if (input_closing(tcp, c, slot, s, r)) return 0;
    if (c->state == TCP_STATE_ESTABLISHED) {
        int result = input_payload(tcp, c, slot, s, r);
        if (result == 2) c->state = TCP_STATE_CLOSE_WAIT;
        return result < 0 ? -1 : 0;
    }
    return -1;
}

int tcp_receive_ipv4(struct tcp_context *tcp, u32 source, u32 destination,
                     const void *data, u32 length, struct tcp_response *r) {
    if (!tcp || !r) return -1;
    r->valid = 0;
    r->connection_id = 0;
    r->option_length = 0;
    r->sack_length = 0;
    struct tcp_segment_view s;
    if (tcp_parse(data, length, &s)) { tcp->stats.malformed++; return -1; }
    if (tcp_checksum_ipv4(source, destination, data, length)) { tcp->stats.checksum_errors++; return -1; }
    tcp->stats.segments_received++;
    struct tcp_connection *c = 0, *listener = 0;
    for (u32 i = 0; i < TCP_CONNECTION_MAX; i++) {
        struct tcp_connection *x = &tcp->connections[i]; if (!x->active || x->family != 4 || x->local_port != s.destination_port) continue;
        if (x->state == TCP_STATE_LISTEN) listener = x;
        if (x->remote_port == s.source_port && x->local_address == destination && x->remote_address == source) { c = x; break; }
    }
    if (!c && listener && (s.flags & TCP_FLAG_SYN) && !(s.flags & TCP_FLAG_ACK)) {
        u32 pending = 0;
        u64 listener_id = id_for(
            listener, (u32)(listener - tcp->connections));
        for (u32 index = 0; index < TCP_CONNECTION_MAX; index++)
            if (tcp->connections[index].active &&
                tcp->connections[index].parent_listener == listener_id)
                pending++;
        if (pending >= listener->listen_backlog) {
            tcp->stats.syn_drops++;
            return -1;
        }
        u32 slot;
        c = allocate(tcp, &slot);
        if (!c) {
            tcp->stats.syn_drops++;
            return -1;
        }
        c->family = 4; c->local_address = destination; c->remote_address = source; c->local_port = s.destination_port; c->remote_port = s.source_port;
        if (s.mss)
            c->remote_mss = s.mss < TCP_RETRANSMIT_DATA_MAX ?
                s.mss : TCP_RETRANSMIT_DATA_MAX;
        c->peer_sack = s.sack_permitted;
        c->send_unacknowledged = next_initial_sequence(tcp, c);
        c->send_next = c->send_unacknowledged + 1;
        c->receive_next = s.sequence + 1; c->receive_window = TCP_RECEIVE_BUFFER_MAX; c->send_window = s.window; c->state = TCP_STATE_SYN_RECEIVED;
        c->parent_listener = id_for(
            listener, (u32)(listener - tcp->connections));
        c->passive = 1;
        c->accepted = 0;
        if (!track_retransmission(
                tcp, c, slot, c->send_unacknowledged,
                TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0, tcp->now)) {
            c->active = 0;
            c->state = TCP_STATE_CLOSED;
            c->timer_version = tcp->timer_epoch++;
            tcp->stats.syn_drops++;
            return -1;
        }
        tcp->stats.connections_opened++;
        r->connection_id = id_for(c, slot);
        response(r, c, TCP_FLAG_SYN | TCP_FLAG_ACK);
        syn_options(tcp, r->options, &r->option_length);
        r->sequence = c->send_unacknowledged;
        return 0;
    }
    if (!c) return -1;
    return tcp_input(tcp, c, &s, r);
}

static int address6_equal(const u8 left[16], const u8 right[16]) {
    for (u32 index = 0; index < 16; index++)
        if (left[index] != right[index]) return 0;
    return 1;
}

int tcp_receive_ipv6(struct tcp_context *tcp,
                     const u8 source[16], const u8 destination[16],
                     const void *data, u32 length,
                     struct tcp_response *r) {
    if (!tcp || !source || !destination || !r) return -1;
    r->valid = 0;
    r->connection_id = 0;
    r->option_length = 0;
    r->sack_length = 0;
    struct tcp_segment_view s;
    if (tcp_parse(data, length, &s)) {
        tcp->stats.malformed++;
        return -1;
    }
    if (tcp_checksum_ipv6(source, destination, data, length)) {
        tcp->stats.checksum_errors++;
        return -1;
    }
    tcp->stats.segments_received++;
    struct tcp_connection *c = 0;
    struct tcp_connection *listener = 0;
    for (u32 index = 0; index < TCP_CONNECTION_MAX; index++) {
        struct tcp_connection *candidate = &tcp->connections[index];
        if (!candidate->active || candidate->family != 6 ||
            candidate->local_port != s.destination_port ||
            !address6_equal(candidate->local_address6, destination))
            continue;
        if (candidate->state == TCP_STATE_LISTEN) {
            listener = candidate;
            continue;
        }
        if (candidate->remote_port == s.source_port &&
            address6_equal(candidate->remote_address6, source)) {
            c = candidate;
            break;
        }
    }
    if (!c && listener && (s.flags & TCP_FLAG_SYN) &&
        !(s.flags & TCP_FLAG_ACK)) {
        u64 listener_id = id_for(
            listener, (u32)(listener - tcp->connections));
        u32 pending = 0;
        for (u32 index = 0; index < TCP_CONNECTION_MAX; index++)
            if (tcp->connections[index].active &&
                tcp->connections[index].parent_listener == listener_id)
                pending++;
        if (pending >= listener->listen_backlog) {
            tcp->stats.syn_drops++;
            return -1;
        }
        u32 slot;
        c = allocate(tcp, &slot);
        if (!c) {
            tcp->stats.syn_drops++;
            return -1;
        }
        c->family = 6;
        for (u32 byte = 0; byte < 16; byte++) {
            c->local_address6[byte] = destination[byte];
            c->remote_address6[byte] = source[byte];
        }
        c->local_port = s.destination_port;
        c->remote_port = s.source_port;
        if (s.mss)
            c->remote_mss = s.mss < TCP_RETRANSMIT_DATA_MAX ?
                s.mss : TCP_RETRANSMIT_DATA_MAX;
        c->peer_sack = s.sack_permitted;
        c->send_unacknowledged = next_initial_sequence(tcp, c);
        c->send_next = c->send_unacknowledged + 1;
        c->receive_next = s.sequence + 1;
        c->receive_window = TCP_RECEIVE_BUFFER_MAX;
        c->send_window = s.window;
        c->state = TCP_STATE_SYN_RECEIVED;
        c->parent_listener = listener_id;
        c->passive = 1;
        if (!track_retransmission(
                tcp, c, slot, c->send_unacknowledged,
                TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0, tcp->now)) {
            c->active = 0;
            c->state = TCP_STATE_CLOSED;
            c->timer_version = tcp->timer_epoch++;
            tcp->stats.syn_drops++;
            return -1;
        }
        tcp->stats.connections_opened++;
        r->connection_id = id_for(c, slot);
        response(r, c, TCP_FLAG_SYN | TCP_FLAG_ACK);
        syn_options(tcp, r->options, &r->option_length);
        r->sequence = c->send_unacknowledged;
        return 0;
    }
    if (!c) return -1;
    return tcp_input(tcp, c, &s, r);
}

int tcp_connection_state(struct tcp_context *tcp, u64 id, u32 *state) {
    struct tcp_connection *c = by_id(tcp, id);
    if (!c || !state) return -1;
    *state = c->state;
    return 0;
}

int tcp_connection_status(struct tcp_context *tcp, u64 id,
                          u32 *state, u32 *readable, u32 *writable,
                          i32 *error, u32 *eof, u32 *granted) {
    struct tcp_connection *connection = by_id(tcp, id);
    if (!connection || !state || !readable || !writable ||
        !error || !eof || !granted)
        return -1;
    *state = connection->state;
    *readable = connection->receive_buffer_length;
    *granted = connection->receive_grant_bytes;
    *writable = !connection->send_fin &&
        (connection->state == TCP_STATE_ESTABLISHED ||
         connection->state == TCP_STATE_CLOSE_WAIT) &&
        connection->send_buffer_length < TCP_SEND_BUFFER_MAX ?
        TCP_SEND_BUFFER_MAX - connection->send_buffer_length : 0;
    *error = connection->error;
    *eof = connection->eof;
    return 0;
}

int tcp_abort(struct tcp_context *tcp, u64 id, i32 error) {
    struct tcp_connection *connection = by_id(tcp, id);
    if (!connection || !error) return -1;
    connection->state = TCP_STATE_CLOSED;
    connection->error = error;
    connection->eof = 1;
    for (u32 index = 0; index < TCP_RETRANSMISSION_MAX; index++)
        connection->retransmissions[index].active = 0;
    release_loans(connection);
    release_grants(connection);
    connection->ack_pending = 0;
    return 0;
}

void tcp_abort_all(struct tcp_context *tcp, i32 error) {
    if (!tcp || !error) return;
    for (u32 index = 0; index < TCP_CONNECTION_MAX; index++) {
        struct tcp_connection *connection = &tcp->connections[index];
        if (!connection->active) continue;
        connection->state = TCP_STATE_CLOSED;
        connection->error = error;
        connection->eof = 1;
        for (u32 entry = 0; entry < TCP_RETRANSMISSION_MAX; entry++)
            connection->retransmissions[entry].active = 0;
        release_loans(connection);
        release_grants(connection);
        connection->ack_pending = 0;
    }
}

int tcp_take_error(struct tcp_context *tcp, u64 id, i32 *error) {
    struct tcp_connection *connection = by_id(tcp, id);
    if (!connection || !error) return -1;
    *error = connection->error;
    connection->error = 0;
    return 0;
}

int tcp_parent_listener(struct tcp_context *tcp, u64 id, u64 *listener_id) {
    struct tcp_connection *connection = by_id(tcp, id);
    if (!connection || !listener_id || !connection->passive ||
        !connection->parent_listener)
        return -1;
    *listener_id = connection->parent_listener;
    return 0;
}

static void fill_transmit(struct tcp_transmit *transmit, u64 id,
                          struct tcp_context *tcp, struct tcp_connection *c,
                          u32 sequence, u8 flags, const u8 *data,
                          u32 length, int retransmission) {
    transmit->connection_id = id;
    transmit->family = c->family;
    transmit->source_address = c->local_address;
    transmit->destination_address = c->remote_address;
    for (u32 byte = 0; byte < 16; byte++) {
        transmit->source_address6[byte] = c->local_address6[byte];
        transmit->destination_address6[byte] = c->remote_address6[byte];
    }
    transmit->sequence = sequence;
    transmit->acknowledgement = c->receive_next;
    transmit->source_port = c->local_port;
    transmit->destination_port = c->remote_port;
    transmit->window = c->receive_window;
    transmit->length = (u16)length;
    transmit->flags = flags;
    transmit->retransmission = retransmission != 0;
    transmit->option_length = 0;
    if (flags & TCP_FLAG_SYN)
        syn_options(tcp, transmit->options, &transmit->option_length);
    else if (c->peer_sack)
        transmit->option_length = write_sack(
            tcp, c, transmit->options, sizeof(transmit->options));
    if (length)
        memcpy(transmit->data, data, length);
}

int tcp_queue_send(struct tcp_context *tcp, u64 id,
                   const void *data, u32 length) {
    struct tcp_connection *c = by_id(tcp, id);
    // Send buffer and page loans share one byte order: never mix them.
    if (!c || c->send_fin || c->send_loan_count ||
        (c->state != TCP_STATE_ESTABLISHED &&
         c->state != TCP_STATE_CLOSE_WAIT) ||
        !data || !length ||
        length > TCP_SEND_BUFFER_MAX - c->send_buffer_length)
        return -1;
    memcpy(c->send_buffer + c->send_buffer_length, data, length);
    c->send_buffer_length += length;
    return 0;
}

int tcp_queue_send_pages(struct tcp_context *tcp, u64 id,
                         struct kernel_object *pages, u32 offset,
                         u32 length) {
    struct tcp_connection *c = by_id(tcp, id);
    struct page_resource *resource = page_resource_get(pages);
    if (!c || !resource || resource->revoked || c->send_fin ||
        (c->state != TCP_STATE_ESTABLISHED &&
         c->state != TCP_STATE_CLOSE_WAIT) ||
        !length || offset >= resource->pages * 4096u ||
        length > resource->pages * 4096u - offset ||
        c->send_buffer_offset < c->send_buffer_length ||
        c->send_loan_count >= TCP_SEND_LOAN_MAX)
        return -1;
    u32 spanned = (offset + length + 4095) / 4096 - offset / 4096;
    if (spanned > TCP_SEND_LOAN_PAGES_MAX - c->send_loan_pages)
        return -1;
    if (page_resource_pin(pages)) return -1;
    struct tcp_send_loan *loan =
        &c->send_loans[(c->send_loan_head + c->send_loan_count) %
                       TCP_SEND_LOAN_MAX];
    loan->pages = pages;
    loan->offset = offset;
    loan->length = length;
    loan->position = 0;
    c->send_loan_count++;
    c->send_loan_pages += spanned;
    return 0;
}

int tcp_queue_receive_pages(struct tcp_context *tcp, u64 id,
                            struct kernel_object *pages, u32 offset,
                            u32 length) {
    struct tcp_connection *c = by_id(tcp, id);
    struct page_resource *resource = page_resource_get(pages);
    if (!c || !resource || resource->revoked || c->eof ||
        (c->state != TCP_STATE_ESTABLISHED &&
         c->state != TCP_STATE_CLOSE_WAIT) ||
        !length || offset >= resource->pages * 4096u ||
        length > resource->pages * 4096u - offset ||
        c->receive_buffer_length ||
        c->receive_grant_count >= TCP_RECEIVE_GRANT_MAX)
        return -1;
    u32 spanned = (offset + length + 4095) / 4096 - offset / 4096;
    if (spanned > TCP_RECEIVE_GRANT_PAGES_MAX - c->receive_grant_pages)
        return -1;
    if (page_resource_pin(pages)) return -1;
    struct tcp_receive_grant *grant =
        &c->receive_grants[(c->receive_grant_head + c->receive_grant_count) %
                           TCP_RECEIVE_GRANT_MAX];
    grant->pages = pages;
    grant->offset = offset;
    grant->length = length;
    grant->position = 0;
    c->receive_grant_count++;
    c->receive_grant_pages += spanned;
    refresh_receive_window(c);
    return 0;
}

int tcp_prepare_transmit(struct tcp_context *tcp, u64 id, u32 now,
                         struct tcp_transmit *transmit) {
    struct tcp_connection *c = by_id(tcp, id);
    if (!c || !transmit) return -1;
    tcp->now = now;
    u32 slot = (u32)(c - tcp->connections);
    if (c->state == TCP_STATE_SYN_SENT) {
        for (u32 index = 0; index < TCP_RETRANSMISSION_MAX; index++)
            if (c->retransmissions[index].active) return 0;
        if (!track_retransmission(tcp, c, slot, c->send_unacknowledged,
                                 TCP_FLAG_SYN, 0, 0, now))
            return -1;
        fill_transmit(transmit, id, tcp, c, c->send_unacknowledged,
                      TCP_FLAG_SYN, 0, 0, 0);
        return 1;
    }
    if (c->state != TCP_STATE_ESTABLISHED &&
        c->state != TCP_STATE_CLOSE_WAIT)
        return 0;
    struct tcp_send_loan *loan = 0;
    const u8 *data = 0;
    u32 available = 0;
    u32 source_position = 0;
    u32 source_total = 0;
    int more_after = 0;
    if (c->send_buffer_offset < c->send_buffer_length) {
        data = c->send_buffer + c->send_buffer_offset;
        available = c->send_buffer_length - c->send_buffer_offset;
        source_position = c->send_buffer_offset;
        source_total = c->send_buffer_length;
        more_after = c->send_loan_count != 0;
    } else if (c->send_loan_count) {
        loan = &c->send_loans[c->send_loan_head];
        data = loan_bytes(loan, loan->position);
        if (!data) return 0;
        available = loan->length - loan->position;
        source_position = loan->position;
        source_total = loan->length;
        more_after = c->send_loan_count > 1;
    }
    if (!data) {
        if (!c->send_fin) return 0;
        u8 flags = TCP_FLAG_FIN | TCP_FLAG_ACK;
        if (!track_retransmission(tcp, c, slot, c->send_next, flags, 0, 0, now))
            return 0;
        fill_transmit(transmit, id, tcp, c, c->send_next, flags, 0, 0, 0);
        c->send_next++;
        c->state = c->state == TCP_STATE_ESTABLISHED ?
            TCP_STATE_FIN_WAIT_1 : TCP_STATE_LAST_ACK;
        tcp->stats.segments_sent++;
        return 1;
    }
    u32 flight = c->send_next - c->send_unacknowledged;
    u32 limit = c->send_window < c->congestion_window ?
        c->send_window : c->congestion_window;
    if (!c->send_window) {
        if (!c->persist_deadline) {
            c->persist_deadline = now + c->persist_interval;
            deadline_push(tcp, slot, TCP_TIMER_PERSIST, 0,
                          c->persist_deadline);
        }
        return 0;
    }
    c->persist_deadline = 0;
    c->persist_interval = TCP_RTO_INITIAL;
    if (flight >= limit) return 0;
    u32 length = available;
    if (length > c->remote_mss) length = c->remote_mss;
    if (length > TCP_RETRANSMIT_DATA_MAX) length = TCP_RETRANSMIT_DATA_MAX;
    if (length > limit - flight) length = limit - flight;
    if (loan) {
        // Segments never span two loan pages: the page frames stay
        // single-owner for the future zero-copy transmit path.
        u32 within = 4096 - (loan->offset + loan->position) % 4096;
        if (length > within) length = within;
    }
    u32 sequence = c->send_next;
    int last = c->send_fin && !more_after &&
        source_position + length == source_total;
    u8 flags = (u8)(TCP_FLAG_ACK | TCP_FLAG_PSH | (last ? TCP_FLAG_FIN : 0));
    if (!length || !track_retransmission(
            tcp, c, slot, sequence, flags, data, length, now))
        return 0;
    fill_transmit(transmit, id, tcp, c, sequence, flags, data, length, 0);
    c->send_next += length;
    if (last) c->send_next++;
    if (!loan) {
        c->send_buffer_offset += length;
        if (c->send_buffer_offset == c->send_buffer_length) {
            c->send_buffer_offset = 0;
            c->send_buffer_length = 0;
        }
    } else {
        loan->position += length;
        if (loan->position == loan->length)
            loan_release(c, loan);
    }
    if (last)
        c->state = c->state == TCP_STATE_ESTABLISHED ?
            TCP_STATE_FIN_WAIT_1 : TCP_STATE_LAST_ACK;
    tcp->stats.bytes_sent += length;
    tcp->stats.segments_sent++;
    return 1;
}

int tcp_receive_data(struct tcp_context *tcp, u64 id,
                     void *buffer, u32 capacity, u32 *received) {
    struct tcp_connection *c = by_id(tcp, id);
    if (!c || !buffer || !capacity || !received) return -1;
    if (!c->receive_buffer_length) {
        *received = 0;
        return c->eof ? 0 : 1;
    }
    u32 count = c->receive_buffer_length < capacity ?
        c->receive_buffer_length : capacity;
    memcpy(buffer, c->receive_buffer, count);
    for (u32 index = count; index < c->receive_buffer_length; index++)
        c->receive_buffer[index - count] = c->receive_buffer[index];
    c->receive_buffer_length -= count;
    refresh_receive_window(c);
    *received = count;
    return 0;
}

int tcp_shutdown(struct tcp_context *tcp, u64 id, u32 now,
                 struct tcp_transmit *transmit) {
    struct tcp_connection *c = by_id(tcp, id);
    if (!c || !transmit || c->send_fin ||
        (c->state != TCP_STATE_ESTABLISHED &&
         c->state != TCP_STATE_CLOSE_WAIT))
        return -1;
    c->send_fin = 1;
    if (c->send_buffer_offset < c->send_buffer_length ||
        c->send_loan_count)
        return 1;
    return tcp_prepare_transmit(tcp, id, now, transmit) == 1 ? 0 : -1;
}

int tcp_detach(struct tcp_context *tcp, u64 id, u32 now,
               struct tcp_transmit *transmit) {
    struct tcp_connection *c = by_id(tcp, id);
    if (!c || !transmit) return -1;
    if (c->state == TCP_STATE_LISTEN || c->state == TCP_STATE_CLOSED)
        return tcp_close(tcp, id);
    if (c->state == TCP_STATE_ESTABLISHED ||
        c->state == TCP_STATE_CLOSE_WAIT) {
        if (!c->send_window &&
            (c->send_buffer_offset < c->send_buffer_length ||
             c->send_loan_count)) {
            fill_transmit(transmit, id, tcp, c, c->send_next,
                          TCP_FLAG_RST | TCP_FLAG_ACK, 0, 0, 0);
            tcp_close(tcp, id);
            return 1;
        }
        int result = tcp_shutdown(tcp, id, now, transmit);
        c->detached = 1;
        if (result < 0) return -1;
        return result == 0 ? 1 : 0;
    }
    if (c->state == TCP_STATE_FIN_WAIT_1 ||
        c->state == TCP_STATE_FIN_WAIT_2 ||
        c->state == TCP_STATE_CLOSING ||
        c->state == TCP_STATE_LAST_ACK ||
        c->state == TCP_STATE_TIME_WAIT) {
        c->detached = 1;
        return 0;
    }
    fill_transmit(transmit, id, tcp, c, c->send_next,
                  TCP_FLAG_RST | TCP_FLAG_ACK, 0, 0, 0);
    tcp_close(tcp, id);
    return 1;
}

static u32 pmtu_overhead(u32 family) {
    return family == 6 ? TCP_PMTU_HEADER6 : TCP_PMTU_HEADER4;
}

static u32 blackhole_next_mtu(const struct tcp_connection *c) {
    u32 mtu = (u32)c->remote_mss + pmtu_overhead(c->family);
    if (c->family == 6) return TCP_PMTU_IPV6_MIN;
    if (mtu > TCP_PMTU_PLATEAU) return TCP_PMTU_PLATEAU;
    return TCP_PMTU_IPV4_MIN;
}

// Rewind the tail of the newest in-flight segment into send_buffer so
// the RTO can emit a MSS-sized prefix. Later in-flight entries keep
// their original size; a new write or the post-RTO flush sends the tail.
static int resegment_to_mss(struct tcp_connection *c,
                            struct tcp_retransmission *e) {
    if (!e->active || e->length <= c->remote_mss) return 0;
    // Rewinding an oversized slot into the send buffer would reorder bytes
    // ahead of unsegmented loan data; resend the slot whole instead.
    if (c->send_loan_count) return 0;
    if (c->send_next != e->end_sequence) return -1;
    u32 keep = c->remote_mss;
    u32 leftover = (u32)e->length - keep;
    u32 fin = (e->flags & TCP_FLAG_FIN) != 0;
    if (leftover > TCP_SEND_BUFFER_MAX - c->send_buffer_length)
        return -1;
    u32 insert = c->send_buffer_offset;
    u32 unsent = c->send_buffer_length - insert;
    for (u32 i = unsent; i > 0; i--)
        c->send_buffer[insert + leftover + i - 1] =
            c->send_buffer[insert + i - 1];
    memcpy(c->send_buffer + insert, e->data + keep, leftover);
    c->send_buffer_length += leftover;
    e->length = (u16)keep;
    if (fin) {
        e->flags = (u8)(e->flags & ~TCP_FLAG_FIN);
        c->send_fin = 1;
        c->send_next--;
    }
    e->end_sequence = e->sequence + keep;
    c->send_next -= leftover;
    return 0;
}

// DF plus a silent next hop looks like consecutive RTOs of full-sized
// segments. Drop to the next plateau and never raise.
static void maybe_blackhole(struct tcp_context *tcp, struct tcp_connection *c,
                            struct tcp_retransmission *e) {
    if (!e->length || e->length < c->remote_mss) {
        c->pmtu_blackhole_rtos = 0;
        return;
    }
    u32 next = blackhole_next_mtu(c);
    u32 overhead = pmtu_overhead(c->family);
    u32 next_mss = next > overhead ? next - overhead : 46;
    if (next_mss >= c->remote_mss) return;
    c->pmtu_blackhole_rtos++;
    if (c->pmtu_blackhole_rtos < TCP_PMTU_BLACKHOLE_RTOS) return;
    tcp_clamp_pmtu(tcp, c->family, c->remote_address, c->remote_address6,
                   next);
    resegment_to_mss(c, e);
    tcp->stats.pmtu_blackholes++;
    c->pmtu_blackhole_rtos = 0;
    if (tcp->pmtu_blackhole)
        tcp->pmtu_blackhole(c->family, c->remote_address, c->remote_address6,
                            next, tcp->pmtu_blackhole_context);
}

int tcp_tick(struct tcp_context *tcp, u32 now,
             struct tcp_transmit *transmit) {
    if (!tcp || !transmit) return -1;
    tcp->now = now;
    for (;;) {
        if (!tcp->deadline_count) break;
        u32 deadline = tcp->deadlines[0].deadline;
        if ((i32)(deadline - now) > 0) break;
        u32 slot = tcp->deadlines[0].slot;
        u32 kind = tcp->deadlines[0].kind;
        u32 entry = tcp->deadlines[0].entry;
        u32 version = tcp->deadlines[0].version;
        deadline_pop(tcp);
        struct tcp_connection *c = &tcp->connections[slot];
        if (!c->active || c->timer_version != version) continue;
        u64 id = id_for(c, slot);
        if (kind == TCP_TIMER_RETRANSMIT) {
            struct tcp_retransmission *entry_item =
                &c->retransmissions[entry];
            if (!entry_item->active ||
                entry_item->deadline != deadline)
                continue;
            if (entry_item->retries >= TCP_RETRY_MAX) {
                c->state = TCP_STATE_CLOSED;
                c->error = -110;
                c->eof = 1;
                entry_item->active = 0;
                if (c->detached) {
                    c->active = 0;
                    c->generation++;
                    if (!c->generation) c->generation = 1;
                    c->timer_version = tcp->timer_epoch++;
                }
                transmit->connection_id = id;
                tcp->stats.retransmission_failures++;
                tcp->stats.connections_closed++;
                return 2;
            }
            entry_item->retries++;
            entry_item->retransmitted = 1;
            tcp->cc->on_rto(c, now);
            u8 fast = entry_item->fast_retransmit;
            entry_item->fast_retransmit = 0;
            if (!fast) maybe_blackhole(tcp, c, entry_item);
            u32 timeout = c->retransmission_timeout << entry_item->retries;
            if (timeout > TCP_RTO_MAX) timeout = TCP_RTO_MAX;
            entry_item->deadline = now + timeout;
            deadline_push(tcp, slot, TCP_TIMER_RETRANSMIT, entry,
                          entry_item->deadline);
            fill_transmit(transmit, id, tcp, c, entry_item->sequence,
                          entry_item->flags, entry_item->data,
                          entry_item->length, 1);
            tcp->stats.retransmissions++;
            tcp->stats.segments_sent++;
            return 1;
        }
        if (kind == TCP_TIMER_PERSIST) {
            if ((c->state != TCP_STATE_ESTABLISHED &&
                 c->state != TCP_STATE_CLOSE_WAIT) ||
                c->send_window ||
                !(c->send_buffer_offset < c->send_buffer_length ||
                  c->send_loan_count) ||
                c->persist_deadline != deadline)
                continue;
            u8 byte;
            if (c->send_buffer_offset < c->send_buffer_length) {
                byte = c->send_buffer[c->send_buffer_offset];
            } else {
                struct tcp_send_loan *loan =
                    &c->send_loans[c->send_loan_head];
                const u8 *probe = loan_bytes(loan, loan->position);
                if (!probe) continue;
                byte = probe[0];
            }
            fill_transmit(transmit, id, tcp, c, c->send_next,
                          TCP_FLAG_ACK, &byte, 1, 0);
            if (c->persist_interval < TCP_RTO_MAX / 2)
                c->persist_interval *= 2;
            else
                c->persist_interval = TCP_RTO_MAX;
            c->persist_deadline = now + c->persist_interval;
            deadline_push(tcp, slot, TCP_TIMER_PERSIST, 0,
                          c->persist_deadline);
            tcp->stats.persist_probes++;
            return 1;
        }
        if (kind == TCP_TIMER_TIME_WAIT) {
            if (c->state != TCP_STATE_TIME_WAIT ||
                c->time_wait_deadline != deadline)
                continue;
            c->active = 0;
            c->state = TCP_STATE_CLOSED;
            c->generation++;
            if (!c->generation) c->generation = 1;
            c->timer_version = tcp->timer_epoch++;
            transmit->connection_id = id;
            tcp->stats.connections_closed++;
            tcp->stats.time_wait_expired++;
            return 2;
        }
        if (kind == TCP_TIMER_ACK_FILTER) {
            if (c->ack_filter_until != deadline) {
                c->ack_filter_until = 0;
                continue;
            }
            if (!c->ack_pending) {
                c->ack_filter_until = 0;
                continue;
            }
            c->ack_pending = 0;
            c->ack_filter_until = 0;
            fill_transmit(transmit, id, tcp, c, c->send_next,
                          TCP_FLAG_ACK, 0, 0, 0);
            tcp->stats.acks_sent++;
            tcp->stats.segments_sent++;
            return 1;
        }
    }
    return 0;
}

int tcp_close(struct tcp_context *tcp, u64 id) {
    struct tcp_connection *c = by_id(tcp, id);
    if (!c) return -1;
    if (c->state == TCP_STATE_LISTEN) {
        for (u32 index = 0; index < TCP_CONNECTION_MAX; index++) {
            struct tcp_connection *child = &tcp->connections[index];
            if (!child->active || child->parent_listener != id || child->accepted)
                continue;
            release_loans(child);
            release_grants(child);
            child->active = 0;
            child->state = TCP_STATE_CLOSED;
            child->generation++;
            if (!child->generation) child->generation = 1;
            child->timer_version = tcp->timer_epoch++;
            tcp->stats.connections_closed++;
        }
    }
    release_loans(c);
    release_grants(c);
    c->active = 0;
    c->state = TCP_STATE_CLOSED;
    c->generation++;
    if (!c->generation) c->generation = 1;
    c->timer_version = tcp->timer_epoch++;
    tcp->stats.connections_closed++;
    return 0;
}

void tcp_set_cc(struct tcp_context *tcp, const struct tcp_cc_ops *cc) {
    if (tcp && cc) tcp->cc = cc;
}

void tcp_set_mss(struct tcp_context *tcp, u16 mss) {
    if (!tcp || mss < 46) return;
    if (mss > TCP_RETRANSMIT_DATA_MAX) mss = TCP_RETRANSMIT_DATA_MAX;
    tcp->mss = mss;
}

void tcp_set_pmtu_blackhole_callback(
    struct tcp_context *tcp,
    void (*callback)(u32 family, u32 address4, const u8 address6[16],
                     u32 ip_mtu, void *context),
    void *context) {
    if (!tcp) return;
    tcp->pmtu_blackhole = callback;
    tcp->pmtu_blackhole_context = context;
}

void tcp_set_ack_filter(struct tcp_context *tcp, u32 enabled) {
    if (tcp) tcp->ack_filter = enabled ? 1 : 0;
}

int tcp_cc_debug(struct tcp_context *tcp, u64 id,
                 u32 *cwnd, u32 *btl_bw, u32 *min_rtt, u32 *phase) {
    struct tcp_connection *c = by_id(tcp, id);
    if (!c || !cwnd || !phase) return -1;
    *cwnd = c->congestion_window;
    *phase = c->bbr_phase;
    if (btl_bw) *btl_bw = c->bbr_bw;
    if (min_rtt) *min_rtt = c->bbr_min_rtt;
    return 0;
}

int tcp_clamp_pmtu(struct tcp_context *tcp, u32 family, u32 address4,
                   const u8 address6[16], u32 ip_mtu) {
    if (!tcp || (family != 4 && family != 6) || ip_mtu < 68) return -1;
    if (family == 6 && !address6) return -1;
    u32 overhead = pmtu_overhead(family);
    u32 mss = ip_mtu > overhead ? ip_mtu - overhead : 46;
    if (mss < 46) mss = 46;
    if (mss > TCP_RETRANSMIT_DATA_MAX) mss = TCP_RETRANSMIT_DATA_MAX;
    u32 clamped = 0;
    for (u32 index = 0; index < TCP_CONNECTION_MAX; index++) {
        struct tcp_connection *c = &tcp->connections[index];
        if (!c->active || c->family != family) continue;
        if (family == 4) {
            if (c->remote_address != address4) continue;
        } else if (!address6_equal(c->remote_address6, address6)) {
            continue;
        }
        if ((u16)mss < c->remote_mss) {
            c->remote_mss = (u16)mss;
            clamped++;
        }
    }
    return clamped ? 0 : 1;
}
