#include "x509_chain.h"
#include "der.h"
#include "sha256.h"
#include "rsa.h"
#include "p256.h"
#include "crypto.h"
#include "trust_store.h"

static u8 lower(u8 value) {
    return value >= 'A' && value <= 'Z' ? (u8)(value + 32u) : value;
}

static int labels_equal(const u8 *left, u32 left_length, const u8 *right,
                        u32 right_length) {
    if (left_length != right_length) return 0;
    for (u32 index = 0; index < left_length; index++)
        if (lower(left[index]) != lower(right[index])) return 0;
    return 1;
}

// A wildcard stands for exactly one label and only the leftmost one, so
// *.example.com covers www.example.com but neither a.b.example.com nor
// example.com itself. A bare *.com would match every host under a registry
// and is refused by requiring at least two labels behind the wildcard.
static int wildcard_matches(const u8 *pattern, u32 pattern_length,
                            const char *host, u32 host_length) {
    if (pattern_length < 2u || pattern[0] != '*' || pattern[1] != '.')
        return 0;
    const u8 *suffix = pattern + 1;
    u32 suffix_length = pattern_length - 1u;
    u32 dots = 0;
    for (u32 index = 0; index < suffix_length; index++)
        if (suffix[index] == '.') dots++;
    if (dots < 2u) return 0;

    u32 first_dot = 0;
    while (first_dot < host_length && host[first_dot] != '.') first_dot++;
    if (first_dot == host_length) return 0;
    // The replaced label must not be empty.
    if (!first_dot) return 0;
    return labels_equal((const u8 *)host + first_dot, host_length - first_dot,
                        suffix, suffix_length);
}

int x509_match_host(const struct x509_certificate *certificate,
                    const char *host, u32 host_length) {
    if (!certificate || !host || !host_length) return -1;
    if (host_length > X509_MAX_NAME_LENGTH) return -1;
    // A trailing dot is legal in DNS and never present in a certificate.
    if (host[host_length - 1u] == '.') host_length--;
    if (!host_length) return -1;

    u32 cursor = 0;
    struct x509_name name;
    while (!x509_san_next(certificate, &cursor, &name)) {
        if (labels_equal(name.data, name.length, (const u8 *)host,
                         host_length))
            return 0;
        if (wildcard_matches(name.data, name.length, host, host_length))
            return 0;
    }
    return -1;
}

static int names_equal(const struct x509_name *left,
                       const struct x509_name *right) {
    return left->length == right->length &&
        crypto_equal(left->data, right->data, left->length);
}

// Splits the DER SEQUENCE { INTEGER r, INTEGER s } an ECDSA signature travels
// in and left-pads both halves to the fixed width the verifier expects.
static int split_ecdsa_signature(const u8 *signature, u32 length, u8 r[32],
                                 u8 s[32]) {
    struct der_reader reader;
    struct der_reader sequence;
    if (der_init(&reader, signature, length)) return -1;
    if (der_read_nested(&reader, DER_TAG_SEQUENCE, &sequence)) return -1;
    if (!der_at_end(&reader)) return -1;
    const u8 *value;
    u32 value_length;
    for (u32 half = 0; half < 2; half++) {
        u8 *out = half ? s : r;
        if (der_read_unsigned(&sequence, &value, &value_length)) return -1;
        if (!value_length || value_length > 32u) return -1;
        for (u32 index = 0; index < 32u; index++) out[index] = 0;
        for (u32 index = 0; index < value_length; index++)
            out[32u - value_length + index] = value[index];
    }
    return der_at_end(&sequence) ? 0 : -1;
}

// Checks that issuer signed subject, using whatever the subject certificate
// says its signature algorithm is. The issuer's key type has to agree with it.
static int signature_is_valid(const struct x509_certificate *subject,
                              const struct x509_certificate *issuer) {
    u8 digest256[SHA256_DIGEST_SIZE];
    u8 digest384[SHA384_DIGEST_SIZE];
    if (subject->signature_algorithm == X509_SIGNATURE_RSA_SHA256 ||
        subject->signature_algorithm == X509_SIGNATURE_ECDSA_SHA256)
        sha256_digest(subject->tbs, subject->tbs_length, digest256);
    else if (subject->signature_algorithm == X509_SIGNATURE_RSA_SHA384 ||
             subject->signature_algorithm == X509_SIGNATURE_ECDSA_SHA384)
        sha384_digest(subject->tbs, subject->tbs_length, digest384);
    else
        return -1;

    if (subject->signature_algorithm == X509_SIGNATURE_RSA_SHA256 ||
        subject->signature_algorithm == X509_SIGNATURE_RSA_SHA384) {
        if (issuer->key_algorithm != X509_KEY_RSA) return -1;
        static struct rsa_public_key key;
        if (rsa_public_key_init(&key, issuer->modulus, issuer->modulus_length,
                                issuer->exponent))
            return -1;
        if (subject->signature_algorithm == X509_SIGNATURE_RSA_SHA256)
            return rsa_pkcs1_verify_sha256(&key, digest256,
                                           subject->signature,
                                           subject->signature_length);
        return rsa_pkcs1_verify_sha384(&key, digest384, subject->signature,
                                       subject->signature_length);
    }

    if (issuer->key_algorithm != X509_KEY_EC_P256) return -1;
    // P-256 keys are not used with SHA-384 in any chain this build accepts,
    // and mixing them would need a digest wider than the curve order.
    if (subject->signature_algorithm != X509_SIGNATURE_ECDSA_SHA256) return -1;
    u8 r[32];
    u8 s[32];
    if (split_ecdsa_signature(subject->signature, subject->signature_length, r,
                              s))
        return -1;
    u8 key[P256_PUBLIC_KEY_SIZE];
    if (issuer->point_length != P256_PUBLIC_KEY_SIZE) return -1;
    for (u32 index = 0; index < P256_PUBLIC_KEY_SIZE; index++)
        key[index] = issuer->point[index];
    return p256_verify(key, digest256, r, s);
}

static int is_valid_issuer(const struct x509_certificate *issuer,
                           u32 certificates_below) {
    // Anything signing another certificate has to say so, both in
    // basicConstraints and, when present, in keyUsage.
    if (!issuer->has_basic_constraints || !issuer->is_ca) return -1;
    if (issuer->has_key_usage &&
        !(issuer->key_usage & X509_KEY_USAGE_KEY_CERT_SIGN))
        return -1;
    // pathLenConstraint counts the intermediates allowed below this CA. A CA
    // that says zero must sit directly above the leaf.
    if (issuer->has_path_length && certificates_below > issuer->path_length)
        return -1;
    return 0;
}

static int within_validity(const struct x509_certificate *certificate,
                           u64 now) {
    if (now < certificate->not_before) return -1;
    if (now > certificate->not_after) return -1;
    return 0;
}

const struct x509_trust_store *x509_builtin_trust_store(void) {
    static struct x509_trust_store store;
    store.anchors = trust_store_anchors;
    store.count = TRUST_STORE_ANCHOR_COUNT;
    return &store;
}

int x509_verify_chain(const u8 *const *chain, const u32 *lengths, u32 count,
                      const struct x509_trust_store *store, const char *host,
                      u32 host_length, u64 now) {
    if (!chain || !lengths || !count || count > X509_MAX_CHAIN) return -1;
    if (!store || !store->anchors || !store->count) return -1;

    // Each parsed certificate is a few hundred bytes and the verifier also
    // needs an anchor plus the RSA scratch below it. That does not fit the
    // modest stack a userspace module gets.
    static struct x509_certificate parsed[X509_MAX_CHAIN];
    for (u32 index = 0; index < count; index++) {
        if (x509_parse(&parsed[index], chain[index], lengths[index]))
            return -1;
        if (within_validity(&parsed[index], now)) return -1;
    }

    if (x509_match_host(&parsed[0], host, host_length)) return -1;
    // A leaf must not be able to sign for anyone else.
    if (parsed[0].has_basic_constraints && parsed[0].is_ca) return -1;

    // Walk up, but try to close on a trust anchor at every level instead of
    // insisting on reaching the last certificate sent. Servers commonly append
    // a cross-signed root whose own issuer is not in this store, and that
    // extra link is redundant once an anchor already signs the level below it.
    for (u32 index = 0; index < count; index++) {
        const struct x509_certificate *current = &parsed[index];
        for (u32 anchor_index = 0; anchor_index < store->count;
             anchor_index++) {
            static struct x509_certificate anchor;
            if (x509_parse(&anchor, store->anchors[anchor_index].der,
                           store->anchors[anchor_index].length))
                continue;
            if (!names_equal(&current->issuer, &anchor.subject)) continue;
            if (is_valid_issuer(&anchor, index)) continue;
            // The anchor's validity still applies, but its signature does not
            // get checked: trust comes from being in this store, not from a
            // self-signature anyone could produce.
            if (within_validity(&anchor, now)) continue;
            if (!signature_is_valid(current, &anchor)) return 0;
        }
        if (index + 1u >= count) break;
        const struct x509_certificate *issuer = &parsed[index + 1u];
        if (!names_equal(&current->issuer, &issuer->subject)) return -1;
        if (is_valid_issuer(issuer, index)) return -1;
        if (signature_is_valid(current, issuer)) return -1;
    }
    return -1;
}
