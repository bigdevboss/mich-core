#ifndef MICH_NET_DNS_MESSAGE_H
#define MICH_NET_DNS_MESSAGE_H

// Pure DNS message encoding and decoding: no sockets, no timers, no
// allocation. The resolver in src/user64/lib/dns.c owns the transport and the
// cache; everything that reads bytes off the wire lives here so the kernel
// test suite can exercise it directly against hostile input.

#define DNS_NAME_MAX 255
#define DNS_LABEL_MAX 63
#define DNS_MESSAGE_MAX 512
#define DNS_TCP_MESSAGE_MAX 4096
#define DNS_ADDRESS_MAX 4
#define DNS_CNAME_MAX 8
#define DNS_JUMP_MAX 16

#define DNS_TYPE_A 1
#define DNS_TYPE_CNAME 5
#define DNS_TYPE_AAAA 28

struct dns_result {
    unsigned int ipv4[DNS_ADDRESS_MAX];
    unsigned char ipv6[DNS_ADDRESS_MAX][16];
    unsigned int ipv4_count;
    unsigned int ipv6_count;
    unsigned int ttl;
};

// Returns the encoded length, or 0 if the name cannot be encoded.
unsigned int dns_build_query(unsigned char *message, unsigned int limit,
                             const char *name, unsigned int type,
                             unsigned int transaction);

// Returns 0 when the reply is accepted, -1 when it must be discarded. A reply
// with the truncation bit set reports 0 with *truncated set and no addresses.
int dns_parse_reply(const unsigned char *message, unsigned int length,
                    const char *name, unsigned int type,
                    unsigned int transaction, unsigned int *truncated,
                    struct dns_result *result);

#endif
