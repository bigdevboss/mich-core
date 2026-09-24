#include <mich/dns.h>
#include "dns_message.h"
#include <mich/event.h>
#include <mich/socket.h>
#include <mich/syscall.h>
#include <mich/timer.h>

struct dns_cache_entry {
    char name[DNS_NAME_MAX + 1];
    struct dns_result result;
    unsigned int type;
    unsigned int expiry;
    unsigned int used;
};

static struct dns_cache_entry dns_cache[MICH_DNS_CACHE_MAX];
static unsigned int dns_server;
static unsigned int dns_interface;
static unsigned int dns_sequence;

// Same construction the DHCP client uses: a cycle counter mixed with RDRAND
// when the CPU reports it. An off-path forgery has to guess this value, so it
// must not come from a plain counter.
static unsigned int name_length(const char *name) {
    unsigned int length = 0;
    while (name && name[length] && length <= DNS_NAME_MAX) length++;
    return length;
}

static int names_equal(const char *left, const char *right) {
    unsigned int index = 0;
    for (; index <= DNS_NAME_MAX; index++) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
        if (!a) return 1;
    }
    return 0;
}

static unsigned int read_be16(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 8) | bytes[1];
}

static void write_be16(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)((value >> 8) & 0xFF);
    bytes[1] = (unsigned char)(value & 0xFF);
}

// Same construction the DHCP client uses: a cycle counter mixed with RDRAND
static unsigned int transaction_id(void) {
    unsigned int low;
    unsigned int high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    unsigned int value = low ^ high ^ (dns_sequence += 0x9E3779B9u);
    unsigned int eax;
    unsigned int ebx;
    unsigned int ecx;
    unsigned int edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));
    if (ecx & (1u << 30)) {
        unsigned int random;
        unsigned char valid;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(random), "=qm"(valid));
        if (valid) value ^= random;
    }
    (void)eax;
    (void)ebx;
    (void)edx;
    return value & 0xFFFF;
}

int mich_dns_init(unsigned int server, unsigned int interface_handle) {
    if (!server) return -1;
    dns_server = server;
    dns_interface = interface_handle;
    mich_dns_cache_flush();
    return 0;
}

void mich_dns_cache_flush(void) {
    for (unsigned int index = 0; index < MICH_DNS_CACHE_MAX; index++) {
        dns_cache[index].used = 0;
        dns_cache[index].expiry = 0;
        dns_cache[index].type = 0;
        dns_cache[index].name[0] = 0;
    }
}

int mich_dns_cache_lookup(const char *name, unsigned int type,
                          struct dns_result *result) {
    if (!name || !result) return -1;
    unsigned int now = mich_ticks();
    for (unsigned int index = 0; index < MICH_DNS_CACHE_MAX; index++) {
        struct dns_cache_entry *entry = &dns_cache[index];
        if (!entry->used || entry->type != type) continue;
        if (!names_equal(entry->name, name)) continue;
        if (now >= entry->expiry) {
            entry->used = 0;
            return -1;
        }
        *result = entry->result;
        return 0;
    }
    return -1;
}

static void cache_store(const char *name, unsigned int type,
                        const struct dns_result *result) {
    if (!result->ttl) return;
    unsigned int now = mich_ticks();
    struct dns_cache_entry *slot = 0;
    for (unsigned int index = 0; index < MICH_DNS_CACHE_MAX; index++) {
        struct dns_cache_entry *entry = &dns_cache[index];
        if (entry->used && entry->type == type &&
            names_equal(entry->name, name)) {
            slot = entry;
            break;
        }
        if (!slot && (!entry->used || now >= entry->expiry)) slot = entry;
    }
    if (!slot) slot = &dns_cache[0];
    unsigned int length = name_length(name);
    for (unsigned int index = 0; index < length; index++)
        slot->name[index] = name[index];
    slot->name[length] = 0;
    slot->result = *result;
    slot->type = type;
    // Ticks run at 100 Hz, so a TTL in seconds becomes a tick deadline.
    slot->expiry = now + result->ttl * 100u;
    slot->used = 1;
}

static int exchange_udp(const unsigned char *query, unsigned int query_length,
                        const char *name, unsigned int type,
                        unsigned int transaction, unsigned int *truncated,
                        struct dns_result *result) {
    int handle = mich_socket_create();
    if (handle <= 0) return -1;
    struct mich_socket_bind_request bind;
    bind.address = 0;
    bind.port = 0;
    bind.reserved = 0;
    int outcome = -1;
    if (!mich_socket_bind((unsigned int)handle, &bind)) {
        static struct mich_socket_send_request request;
        static struct mich_socket_receive_result reply;
        request.destination_address = dns_server;
        request.destination_port = 53;
        request.length = (unsigned short)query_length;
        for (unsigned int index = 0; index < MICH_SOCKET_PAYLOAD_MAX; index++)
            request.payload[index] = index < query_length ? query[index] : 0;
        for (unsigned int attempt = 0;
             attempt < MICH_DNS_RETRY_MAX && outcome; attempt++) {
            if (mich_socket_send_to((unsigned int)handle, &request)) break;
            unsigned int deadline = mich_ticks() + MICH_DNS_TIMEOUT_TICKS;
            while (mich_ticks() < deadline) {
                if (mich_socket_receive_from((unsigned int)handle, &reply)) {
                    mich_yield();
                    continue;
                }
                if (reply.source_address != dns_server ||
                    reply.source_port != 53)
                    continue;
                if (!dns_parse_reply(reply.payload, reply.length, name,
                                          type, transaction, truncated,
                                          result)) {
                    outcome = 0;
                    break;
                }
            }
        }
    }
    mich_handle_close((unsigned int)handle);
    return outcome;
}

// connect only queues the SYN. Sending before the handshake completes drops the
// query into a socket that is still SYN_SENT, so the reply never comes and the
// exchange dies on the receive timeout instead of reporting the real reason.
static int wait_connected(unsigned int handle) {
    unsigned int deadline = mich_ticks() + MICH_DNS_TCP_TIMEOUT_TICKS;
    while (mich_ticks() < deadline) {
        struct mich_socket_stream_state_result state;
        state.readiness = 0;
        if (mich_socket_stream_state(handle, &state)) return -1;
        if (state.readiness & SOCKET_READY_ERROR) return -1;
        if (state.readiness & SOCKET_READY_HANGUP) return -1;
        if (state.readiness & SOCKET_READY_CONNECTED) return 0;
        mich_yield();
    }
    return -1;
}

static int exchange_tcp(const unsigned char *query, unsigned int query_length,
                        const char *name, unsigned int type,
                        unsigned int transaction,
                        struct dns_result *result) {
    int handle = mich_socket_stream_create();
    if (handle <= 0) return -1;
    struct mich_socket_stream_connect_request connect;
    // Zero means the kernel picks the interface from the route table, which is
    // the only option for a process that does not own one. A driver capsule
    // passes the handle it owns.
    connect.interface_handle = dns_interface;
    connect.destination_address = dns_server;
    connect.destination_port = 53;
    connect.reserved = 0;
    int outcome = -1;
    static unsigned char framed[DNS_TCP_MESSAGE_MAX];
    if (!mich_socket_stream_connect((unsigned int)handle, &connect) &&
        !wait_connected((unsigned int)handle)) {
        struct mich_socket_stream_data data;
        // A DNS message over TCP carries a two-byte length prefix, so the
        // prefix has to be validated before any body byte is trusted.
        data.length = query_length + 2;
        data.reserved = 0;
        write_be16(data.data, query_length);
        for (unsigned int index = 0; index < query_length; index++)
            data.data[index + 2] = query[index];
        if (!mich_socket_stream_send((unsigned int)handle, &data)) {
            unsigned int total = 0;
            unsigned int expected = 0;
            unsigned int deadline =
                mich_ticks() + MICH_DNS_TCP_TIMEOUT_TICKS;
            while (mich_ticks() < deadline) {
                struct mich_socket_stream_state_result state;
                state.readiness = 0;
                state.eof = 0;
                if (mich_socket_stream_state((unsigned int)handle, &state))
                    break;
                if (state.readiness & SOCKET_READY_ERROR) break;
                // Frames arrive through a separate driver process, so every
                // turn has to yield. Reading before the socket reports data
                // just spins and starves the task that would deliver it.
                if (!(state.readiness & SOCKET_READY_READABLE)) {
                    if (state.eof) break;
                    mich_yield();
                    continue;
                }
                struct mich_socket_stream_data chunk;
                chunk.length = 0;
                chunk.reserved = 0;
                if (mich_socket_stream_receive((unsigned int)handle, &chunk) ||
                    !chunk.length) {
                    mich_yield();
                    continue;
                }
                for (unsigned int index = 0;
                     index < chunk.length && total < sizeof(framed); index++)
                    framed[total++] = chunk.data[index];
                if (total >= 2 && !expected) {
                    expected = read_be16(framed);
                    if (!expected || expected > DNS_TCP_MESSAGE_MAX - 2)
                        break;
                }
                if (expected && total >= expected + 2) {
                    outcome = dns_parse_reply(framed + 2, expected, name,
                                                   type, transaction, 0,
                                                   result);
                    break;
                }
            }
        }
    }
    mich_handle_close((unsigned int)handle);
    return outcome;
}

int mich_dns_resolve(const char *name, unsigned int type,
                     struct dns_result *result) {
    if (!name || !result || !dns_server) return -1;
    if (type != DNS_TYPE_A && type != DNS_TYPE_AAAA) return -1;
    unsigned int length = name_length(name);
    if (!length || length > DNS_NAME_MAX) return -1;
    if (!mich_dns_cache_lookup(name, type, result)) return 0;
    unsigned char query[DNS_MESSAGE_MAX];
    unsigned int transaction = transaction_id();
    unsigned int query_length =
        dns_build_query(query, sizeof(query), name, type, transaction);
    if (!query_length) return -1;
    unsigned int truncated = 0;
    int outcome = exchange_udp(query, query_length, name, type, transaction,
                               &truncated, result);
    // Truncation is not the only reason to retry over TCP. A UDP exchange that
    // never got an answer, because the datagram was lost or the path blocks
    // port 53 over UDP, has to fall back too, otherwise the resolver gives up
    // while a working transport is still available.
    if (outcome || truncated)
        outcome = exchange_tcp(query, query_length, name, type, transaction,
                               result);
    if (outcome) return -1;
    cache_store(name, type, result);
    return 0;
}
