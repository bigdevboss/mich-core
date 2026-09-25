#include "x509.h"
#include "der.h"

#define TAG_VERSION (DER_TAG_CONTEXT | DER_TAG_CONSTRUCTED | 0u)
#define TAG_EXTENSIONS (DER_TAG_CONTEXT | DER_TAG_CONSTRUCTED | 3u)
#define TAG_SAN_DNS (DER_TAG_CONTEXT | 2u)

// Algorithm identifiers are compared as encoded bytes. Decoding an OID into
// numbers would only be needed to print it, and this code never does.
static const u8 oid_rsa_sha256[9] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B,
};

static const u8 oid_ecdsa_sha256[8] = {
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02,
};

static const u8 oid_rsa_encryption[9] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01,
};

static const u8 oid_ec_public_key[7] = {
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01,
};

static const u8 oid_prime256v1[8] = {
    0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07,
};

static const u8 oid_basic_constraints[3] = { 0x55, 0x1D, 0x13 };
static const u8 oid_key_usage[3] = { 0x55, 0x1D, 0x0F };
static const u8 oid_subject_alt_name[3] = { 0x55, 0x1D, 0x11 };

static int oid_is(const u8 *value, u32 length, const u8 *expected,
                  u32 expected_length) {
    if (length != expected_length) return 0;
    for (u32 index = 0; index < length; index++)
        if (value[index] != expected[index]) return 0;
    return 1;
}

static int digits(const u8 *text, u32 count, u32 *out) {
    u32 value = 0;
    for (u32 index = 0; index < count; index++) {
        if (text[index] < '0' || text[index] > '9') return -1;
        value = value * 10u + (u32)(text[index] - '0');
    }
    *out = value;
    return 0;
}

// Days since 1970-01-01 for a proleptic Gregorian date. Duplicated from the
// RTC driver on purpose: this file has to stay free of arch headers.
static u64 days_from_civil(u32 year, u32 month, u32 day) {
    u32 shifted = year - (month <= 2 ? 1u : 0u);
    u32 era = shifted / 400u;
    u32 year_of_era = shifted - era * 400u;
    u32 day_of_year =
        (153u * (month + (month > 2 ? (u32)-3 : 9u)) + 2u) / 5u + day - 1u;
    u32 day_of_era = year_of_era * 365u + year_of_era / 4u -
        year_of_era / 100u + day_of_year;
    return (u64)era * 146097ull + (u64)day_of_era - 719468ull;
}

// UTCTime carries a two digit year and GeneralizedTime a four digit one.
// Getting the pivot wrong is how an expired certificate becomes valid, so the
// rule from RFC 5280 is spelled out: 50 and above is 19xx.
static int parse_time(u8 tag, const u8 *text, u32 length, u64 *out) {
    u32 year;
    u32 offset;
    if (tag == DER_TAG_UTC_TIME) {
        if (length != 13u || text[12] != 'Z') return -1;
        u32 short_year;
        if (digits(text, 2, &short_year)) return -1;
        year = short_year >= 50u ? 1900u + short_year : 2000u + short_year;
        offset = 2;
    } else if (tag == DER_TAG_GENERALIZED_TIME) {
        if (length != 15u || text[14] != 'Z') return -1;
        if (digits(text, 4, &year)) return -1;
        offset = 4;
    } else {
        return -1;
    }
    u32 month;
    u32 day;
    u32 hour;
    u32 minute;
    u32 second;
    if (digits(text + offset, 2, &month) ||
        digits(text + offset + 2, 2, &day) ||
        digits(text + offset + 4, 2, &hour) ||
        digits(text + offset + 6, 2, &minute) ||
        digits(text + offset + 8, 2, &second))
        return -1;
    if (year < 1970u || month < 1u || month > 12u || day < 1u || day > 31u ||
        hour > 23u || minute > 59u || second > 60u)
        return -1;
    *out = days_from_civil(year, month, day) * 86400ull +
        (u64)hour * 3600ull + (u64)minute * 60ull + (u64)second;
    return 0;
}

static int parse_algorithm(struct der_reader *reader, u32 *algorithm) {
    struct der_reader sequence;
    if (der_read_nested(reader, DER_TAG_SEQUENCE, &sequence)) return -1;
    const u8 *oid;
    u32 oid_length;
    if (der_read(&sequence, DER_TAG_OID, &oid, &oid_length)) return -1;
    if (oid_is(oid, oid_length, oid_rsa_sha256, sizeof(oid_rsa_sha256)))
        *algorithm = X509_SIGNATURE_RSA_SHA256;
    else if (oid_is(oid, oid_length, oid_ecdsa_sha256,
                    sizeof(oid_ecdsa_sha256)))
        *algorithm = X509_SIGNATURE_ECDSA_SHA256;
    else
        *algorithm = X509_SIGNATURE_UNKNOWN;
    return 0;
}

static int parse_public_key(struct der_reader *reader,
                            struct x509_certificate *out) {
    struct der_reader spki;
    if (der_read_nested(reader, DER_TAG_SEQUENCE, &spki)) return -1;
    struct der_reader algorithm;
    if (der_read_nested(&spki, DER_TAG_SEQUENCE, &algorithm)) return -1;
    const u8 *oid;
    u32 oid_length;
    if (der_read(&algorithm, DER_TAG_OID, &oid, &oid_length)) return -1;

    const u8 *bits;
    u32 bits_length;
    if (oid_is(oid, oid_length, oid_rsa_encryption,
               sizeof(oid_rsa_encryption))) {
        out->key_algorithm = X509_KEY_RSA;
        if (der_read(&spki, DER_TAG_BIT_STRING, &bits, &bits_length)) return -1;
        // The first content byte of a BIT STRING counts unused trailing bits,
        // and a key never has any.
        if (!bits_length || bits[0]) return -1;
        struct der_reader key;
        if (der_init(&key, bits + 1, bits_length - 1u)) return -1;
        struct der_reader sequence;
        if (der_read_nested(&key, DER_TAG_SEQUENCE, &sequence)) return -1;
        if (der_read_unsigned(&sequence, &out->modulus, &out->modulus_length))
            return -1;
        if (der_read_u32(&sequence, &out->exponent)) return -1;
        if (!der_at_end(&sequence)) return -1;
    } else if (oid_is(oid, oid_length, oid_ec_public_key,
                      sizeof(oid_ec_public_key))) {
        const u8 *curve;
        u32 curve_length;
        if (der_read(&algorithm, DER_TAG_OID, &curve, &curve_length)) return -1;
        // A curve this build cannot verify is reported rather than refused:
        // the parser says what the certificate contains, and the chain
        // validator decides whether that is usable.
        int is_p256 = oid_is(curve, curve_length, oid_prime256v1,
                             sizeof(oid_prime256v1));
        if (der_read(&spki, DER_TAG_BIT_STRING, &bits, &bits_length)) return -1;
        if (!bits_length || bits[0]) return -1;
        if (is_p256 && bits_length == 66u && bits[1] == 0x04u) {
            out->key_algorithm = X509_KEY_EC_P256;
            out->point = bits + 1;
            out->point_length = bits_length - 1u;
        } else {
            out->key_algorithm = X509_KEY_UNKNOWN;
        }
    } else {
        out->key_algorithm = X509_KEY_UNKNOWN;
        if (der_read(&spki, DER_TAG_BIT_STRING, &bits, &bits_length)) return -1;
    }
    return 0;
}

static int parse_basic_constraints(const u8 *value, u32 length,
                                   struct x509_certificate *out) {
    struct der_reader reader;
    struct der_reader sequence;
    if (der_init(&reader, value, length)) return -1;
    if (der_read_nested(&reader, DER_TAG_SEQUENCE, &sequence)) return -1;
    out->has_basic_constraints = 1;
    out->is_ca = 0;
    if (!der_at_end(&sequence)) {
        u8 tag;
        if (der_peek(&sequence, &tag)) return -1;
        if (tag == DER_TAG_BOOLEAN) {
            const u8 *flag;
            u32 flag_length;
            if (der_read(&sequence, DER_TAG_BOOLEAN, &flag, &flag_length))
                return -1;
            if (flag_length != 1u) return -1;
            // DER allows exactly two encodings for a boolean.
            if (flag[0] != 0x00u && flag[0] != 0xFFu) return -1;
            out->is_ca = flag[0] == 0xFFu;
        }
    }
    if (!der_at_end(&sequence)) {
        if (der_read_u32(&sequence, &out->path_length)) return -1;
        out->has_path_length = 1;
    }
    return der_at_end(&sequence) ? 0 : -1;
}

static int parse_key_usage(const u8 *value, u32 length,
                           struct x509_certificate *out) {
    struct der_reader reader;
    const u8 *bits;
    u32 bits_length;
    if (der_init(&reader, value, length)) return -1;
    if (der_read(&reader, DER_TAG_BIT_STRING, &bits, &bits_length)) return -1;
    if (!bits_length || bits[0] > 7u) return -1;
    out->has_key_usage = 1;
    out->key_usage = bits_length > 1u ? bits[1] : 0u;
    return 0;
}

static int parse_subject_alt_name(const u8 *value, u32 length,
                                  struct x509_certificate *out) {
    struct der_reader reader;
    struct der_reader sequence;
    if (der_init(&reader, value, length)) return -1;
    // Validate the shape now, so a malformed list is rejected with the rest of
    // the certificate rather than at host-matching time.
    if (der_read_nested(&reader, DER_TAG_SEQUENCE, &sequence)) return -1;
    while (!der_at_end(&sequence)) {
        u8 tag;
        const u8 *name;
        u32 name_length;
        if (der_read_any(&sequence, &tag, &name, &name_length)) return -1;
        if (tag == TAG_SAN_DNS && name_length > X509_MAX_NAME_LENGTH)
            return -1;
    }
    out->san = value;
    out->san_length = length;
    return 0;
}

int x509_san_next(const struct x509_certificate *certificate, u32 *cursor,
                  struct x509_name *name) {
    if (!certificate || !cursor || !name || !certificate->san) return -1;
    struct der_reader reader;
    struct der_reader sequence;
    if (der_init(&reader, certificate->san, certificate->san_length))
        return -1;
    if (der_read_nested(&reader, DER_TAG_SEQUENCE, &sequence)) return -1;
    if (*cursor > sequence.end) return -1;
    sequence.offset = *cursor;
    while (!der_at_end(&sequence)) {
        u8 tag;
        const u8 *value;
        u32 length;
        if (der_read_any(&sequence, &tag, &value, &length)) return -1;
        if (tag != TAG_SAN_DNS) continue;
        name->data = value;
        name->length = length;
        *cursor = sequence.offset;
        return 0;
    }
    *cursor = sequence.offset;
    return -1;
}

static int parse_extensions(struct der_reader *tbs,
                            struct x509_certificate *out) {
    struct der_reader wrapper;
    if (der_read_nested(tbs, TAG_EXTENSIONS, &wrapper)) return -1;
    struct der_reader list;
    if (der_read_nested(&wrapper, DER_TAG_SEQUENCE, &list)) return -1;
    while (!der_at_end(&list)) {
        struct der_reader extension;
        if (der_read_nested(&list, DER_TAG_SEQUENCE, &extension)) return -1;
        const u8 *oid;
        u32 oid_length;
        if (der_read(&extension, DER_TAG_OID, &oid, &oid_length)) return -1;
        u8 tag;
        if (!der_peek(&extension, &tag) && tag == DER_TAG_BOOLEAN) {
            const u8 *critical;
            u32 critical_length;
            if (der_read(&extension, DER_TAG_BOOLEAN, &critical,
                         &critical_length))
                return -1;
            if (critical_length != 1u) return -1;
            if (critical[0] != 0x00u && critical[0] != 0xFFu) return -1;
        }
        const u8 *body;
        u32 body_length;
        if (der_read(&extension, DER_TAG_OCTET_STRING, &body, &body_length))
            return -1;
        if (oid_is(oid, oid_length, oid_basic_constraints,
                   sizeof(oid_basic_constraints))) {
            if (parse_basic_constraints(body, body_length, out)) return -1;
        } else if (oid_is(oid, oid_length, oid_key_usage,
                          sizeof(oid_key_usage))) {
            if (parse_key_usage(body, body_length, out)) return -1;
        } else if (oid_is(oid, oid_length, oid_subject_alt_name,
                          sizeof(oid_subject_alt_name))) {
            if (parse_subject_alt_name(body, body_length, out)) return -1;
        }
    }
    return 0;
}

int x509_parse(struct x509_certificate *out, const u8 *data, u32 length) {
    if (!out || !data || !length) return -1;
    if (length > X509_MAX_CERTIFICATE_BYTES) return -1;

    u8 *bytes = (u8 *)out;
    for (u32 index = 0; index < sizeof(*out); index++) bytes[index] = 0;

    struct der_reader reader;
    if (der_init(&reader, data, length)) return -1;
    struct der_reader certificate;
    if (der_read_nested(&reader, DER_TAG_SEQUENCE, &certificate)) return -1;
    if (!der_at_end(&reader)) return -1;

    // The signature covers the TBS element including its header, so it is
    // captured as raw bytes before anything inside is looked at.
    const u8 *tbs_value;
    u32 tbs_value_length;
    if (der_read_element(&certificate, DER_TAG_SEQUENCE, &out->tbs,
                         &out->tbs_length, &tbs_value, &tbs_value_length))
        return -1;

    u32 outer_algorithm;
    if (parse_algorithm(&certificate, &outer_algorithm)) return -1;
    const u8 *signature;
    u32 signature_length;
    if (der_read(&certificate, DER_TAG_BIT_STRING, &signature,
                 &signature_length))
        return -1;
    if (!signature_length || signature[0]) return -1;
    out->signature = signature + 1;
    out->signature_length = signature_length - 1u;
    if (!der_at_end(&certificate)) return -1;

    struct der_reader tbs;
    if (der_init(&tbs, tbs_value, tbs_value_length)) return -1;

    u8 tag;
    if (der_peek(&tbs, &tag)) return -1;
    if (tag == TAG_VERSION) {
        struct der_reader version;
        if (der_read_nested(&tbs, TAG_VERSION, &version)) return -1;
        u32 number;
        if (der_read_u32(&version, &number)) return -1;
        // Only v3 carries extensions, and every certificate in use is v3.
        if (number != 2u) return -1;
        if (!der_at_end(&version)) return -1;
    } else {
        return -1;
    }

    const u8 *serial;
    u32 serial_length;
    if (der_read(&tbs, DER_TAG_INTEGER, &serial, &serial_length)) return -1;
    if (!serial_length) return -1;

    if (parse_algorithm(&tbs, &out->signature_algorithm)) return -1;
    // The algorithm named inside the signed body has to match the one outside
    // it, or an attacker chooses which the verifier believes.
    if (out->signature_algorithm != outer_algorithm) return -1;

    const u8 *issuer_value;
    u32 issuer_value_length;
    if (der_read_element(&tbs, DER_TAG_SEQUENCE, &out->issuer.data,
                         &out->issuer.length, &issuer_value,
                         &issuer_value_length))
        return -1;

    struct der_reader validity;
    if (der_read_nested(&tbs, DER_TAG_SEQUENCE, &validity)) return -1;
    u8 time_tag;
    const u8 *time_value;
    u32 time_length;
    if (der_read_any(&validity, &time_tag, &time_value, &time_length))
        return -1;
    if (parse_time(time_tag, time_value, time_length, &out->not_before))
        return -1;
    if (der_read_any(&validity, &time_tag, &time_value, &time_length))
        return -1;
    if (parse_time(time_tag, time_value, time_length, &out->not_after))
        return -1;
    if (!der_at_end(&validity)) return -1;
    if (out->not_after < out->not_before) return -1;

    const u8 *subject_value;
    u32 subject_value_length;
    if (der_read_element(&tbs, DER_TAG_SEQUENCE, &out->subject.data,
                         &out->subject.length, &subject_value,
                         &subject_value_length))
        return -1;

    if (parse_public_key(&tbs, out)) return -1;

    while (!der_at_end(&tbs)) {
        if (der_peek(&tbs, &tag)) return -1;
        if (tag == TAG_EXTENSIONS) {
            if (parse_extensions(&tbs, out)) return -1;
            continue;
        }
        // issuerUniqueID and subjectUniqueID are obsolete but legal.
        const u8 *skipped;
        u32 skipped_length;
        if (der_read_any(&tbs, &tag, &skipped, &skipped_length)) return -1;
    }
    return 0;
}
