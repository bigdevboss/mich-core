#ifndef MICH_CRYPTO_X509_H
#define MICH_CRYPTO_X509_H

#include "types.h"

#define X509_MAX_CERTIFICATE_BYTES 8192u
#define X509_MAX_DEPTH 16u
#define X509_MAX_NAME_LENGTH 255u

#define X509_SIGNATURE_UNKNOWN 0u
#define X509_SIGNATURE_RSA_SHA256 1u
#define X509_SIGNATURE_ECDSA_SHA256 2u
#define X509_SIGNATURE_RSA_SHA384 3u
#define X509_SIGNATURE_ECDSA_SHA384 4u

#define X509_KEY_UNKNOWN 0u
#define X509_KEY_RSA 1u
#define X509_KEY_EC_P256 2u

#define X509_KEY_USAGE_DIGITAL_SIGNATURE 0x80u
#define X509_KEY_USAGE_KEY_CERT_SIGN 0x04u

struct x509_name {
    const u8 *data;
    u32 length;
};

// Every span points into the buffer handed to x509_parse, which has to outlive
// this structure. Nothing is copied: the signature covers exact bytes, and a
// re-encoded field would not hash to the same value.
struct x509_certificate {
    const u8 *tbs;
    u32 tbs_length;

    struct x509_name issuer;
    struct x509_name subject;

    u64 not_before;
    u64 not_after;

    u32 signature_algorithm;
    const u8 *signature;
    u32 signature_length;

    u32 key_algorithm;
    // RSA
    const u8 *modulus;
    u32 modulus_length;
    u32 exponent;
    // EC, SEC 1 uncompressed point
    const u8 *point;
    u32 point_length;

    int has_basic_constraints;
    int is_ca;
    int has_path_length;
    u32 path_length;

    int has_key_usage;
    u32 key_usage;

    int has_extended_key_usage;
    int eku_server_auth;
    int eku_any;

    // Name Constraints is kept as raw DER and walked while the chain is built,
    // where the leaf names to test against are known. It is always critical, so
    // reaching this field means the parser was required to recognize it.
    const u8 *name_constraints;
    u32 name_constraints_length;

    // The SAN extension is kept as raw DER and walked on demand. Google's
    // certificate carries 65 names, so any fixed array here would be either
    // wasteful or wrong, and the only question ever asked of this field is
    // whether one particular host matches.
    const u8 *san;
    u32 san_length;
};

// Parses one DER certificate. Returns 0 on success, -1 on anything malformed;
// the input comes from a peer that has not been authenticated yet, so every
// rejection is expected traffic rather than an exceptional case.
int x509_parse(struct x509_certificate *out, const u8 *data, u32 length);

// Walks the dNSName entries of the SAN extension. Pass 0 in cursor to start;
// it is advanced past the entry returned. Returns 0 while names remain, -1
// when the list is exhausted or malformed.
int x509_san_next(const struct x509_certificate *certificate, u32 *cursor,
                  struct x509_name *name);

// Splits the Name Constraints extension into its permitted and excluded subtree
// spans. A span with a null data pointer means that list is absent. Returns 0
// when the certificate carries a well-formed extension, -1 when it has none or
// the encoding is malformed.
int x509_name_constraints(const struct x509_certificate *certificate,
                          struct x509_name *permitted,
                          struct x509_name *excluded);

// Walks the dNSName entries of a subtree span returned above. Pass 0 in cursor
// to start; it advances past the entry returned. Returns 0 while names remain,
// -1 once the list is exhausted.
int x509_dns_constraint_next(const struct x509_name *subtrees, u32 *cursor,
                             struct x509_name *name);

#endif
