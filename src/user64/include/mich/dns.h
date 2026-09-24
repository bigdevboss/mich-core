#ifndef MICH64_USER_DNS_H
#define MICH64_USER_DNS_H

#include "dns_message.h"

// Resolver policy lives here; message encoding lives in src/net/dns_message.h.
#define MICH_DNS_CACHE_MAX 16
#define MICH_DNS_RETRY_MAX 3
#define MICH_DNS_TIMEOUT_TICKS 200

int mich_dns_init(unsigned int server, unsigned int interface_handle);
int mich_dns_resolve(const char *name, unsigned int type,
                     struct dns_result *result);
int mich_dns_cache_lookup(const char *name, unsigned int type,
                          struct dns_result *result);
void mich_dns_cache_flush(void);

#endif
