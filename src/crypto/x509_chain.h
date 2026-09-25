#ifndef MICH_CRYPTO_X509_CHAIN_H
#define MICH_CRYPTO_X509_CHAIN_H

#include "types.h"
#include "x509.h"

// Root, one cross-signed intermediate, one working intermediate and the leaf
// is the deepest arrangement in use. A longer chain is refused rather than
// walked, which also bounds the work an unauthenticated peer can ask for.
#define X509_MAX_CHAIN 4u

struct x509_trust_anchor {
    const u8 *der;
    u32 length;
};

struct x509_trust_store {
    const struct x509_trust_anchor *anchors;
    u32 count;
};

// The trust store built into this image.
const struct x509_trust_store *x509_builtin_trust_store(void);

// Matches a host name against the SAN entries of a certificate following
// RFC 6125: case-insensitive, dNSName only, and a wildcard may replace the
// leftmost label and nothing else. The deprecated commonName is not consulted.
// Returns 0 on a match.
int x509_match_host(const struct x509_certificate *certificate,
                    const char *host, u32 host_length);

// Verifies a certificate chain as sent by a TLS server: leaf first, issuers
// after it, root omitted. now is Unix seconds supplied by the caller, because
// this code carries no clock of its own and tests need a fixed moment.
//
// Returns 0 when the chain is trusted for host, -1 otherwise.
int x509_verify_chain(const u8 *const *chain, const u32 *lengths, u32 count,
                      const struct x509_trust_store *store, const char *host,
                      u32 host_length, u64 now);

#endif
