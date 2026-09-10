#ifndef LOOPBACK_H
#define LOOPBACK_H

#include "types.h"
#include "object.h"
#include "ipv4.h"
#include "net_abi.h"

#define LOOPBACK_MTU (NET_PACKET_DATA_MAX - IPV4_HEADER_MIN)

struct loopback_stats {
    u64 transmitted;
    u64 received;
    u64 bytes;
    u64 drops;
};

struct loopback_context {
    struct ipv4_context *ipv4;
    struct kernel_object *pool;
    u16 identification;
    u32 active;
    struct loopback_stats stats;
};

int loopback_init(struct loopback_context *loopback,
                  struct ipv4_context *ipv4, u32 buffer_count);
void loopback_destroy(struct loopback_context *loopback);
int loopback_transmit(struct ipv4_context *ipv4, u32 destination,
                      u8 protocol, const void *payload,
                      u32 payload_length, void *context);

#endif
