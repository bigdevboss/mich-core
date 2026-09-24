#ifndef MICH64_USER_DNS_H
#define MICH64_USER_DNS_H

#include "dns_message.h"

// Resolver policy lives here; message encoding lives in src/net/dns_message.h.
#define MICH_DNS_CACHE_MAX 16
#define MICH_DNS_RETRY_MAX 3
#define MICH_DNS_TIMEOUT_TICKS 200
// A TCP exchange needs a handshake, a send and a reply, and each turn of the
// receive loop yields to the driver task that delivers the frames. Under TCG
// that is roughly five yields per tick, so the datagram budget is far too
// small here and the exchange would time out with the answer still in flight.
#define MICH_DNS_TCP_TIMEOUT_TICKS 4000

// interface_handle may be 0, in which case every exchange is routed by the
// kernel. Only a driver capsule that owns an interface passes a real handle.
int mich_dns_init(unsigned int server, unsigned int interface_handle);
int mich_dns_resolve(const char *name, unsigned int type,
                     struct dns_result *result);
int mich_dns_cache_lookup(const char *name, unsigned int type,
                          struct dns_result *result);
void mich_dns_cache_flush(void);

#endif
