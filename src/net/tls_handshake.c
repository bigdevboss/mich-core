#include "tls_handshake.h"
#include "x25519.h"
#include "p256.h"
#include "rsa.h"
#include "der.h"
#include "crypto.h"

#define HANDSHAKE_CLIENT_HELLO 1u
#define HANDSHAKE_SERVER_HELLO 2u
#define HANDSHAKE_NEW_SESSION_TICKET 4u
#define HANDSHAKE_ENCRYPTED_EXTENSIONS 8u
#define HANDSHAKE_CERTIFICATE 11u
#define HANDSHAKE_CERTIFICATE_VERIFY 15u
#define HANDSHAKE_FINISHED 20u

#define EXTENSION_SERVER_NAME 0u
#define EXTENSION_SUPPORTED_GROUPS 10u
#define EXTENSION_SIGNATURE_ALGORITHMS 13u
#define EXTENSION_SUPPORTED_VERSIONS 43u
#define EXTENSION_KEY_SHARE 51u

#define GROUP_X25519 0x001Du
#define CIPHER_AES_128_GCM_SHA256 0x1301u
#define SIGNATURE_ECDSA_SECP256R1_SHA256 0x0403u
#define SIGNATURE_RSA_PSS_RSAE_SHA256 0x0804u
#define SIGNATURE_RSA_PKCS1_SHA256 0x0401u

// RFC 8446 section 4.4.3: the signature covers this prefix, not the transcript
// alone, so a signature taken here cannot be replayed into another protocol
// that signs a bare hash.
static const u8 certificate_verify_context[] = {
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20,
    'T', 'L', 'S', ' ', '1', '.', '3', ',', ' ', 's', 'e', 'r', 'v', 'e', 'r',
    ' ', 'C', 'e', 'r', 't', 'i', 'f', 'i', 'c', 'a', 't', 'e', 'V', 'e', 'r',
    'i', 'f', 'y', 0x00,
};

struct writer {
    u8 *data;
    u32 offset;
    u32 limit;
    int overflow;
};

static void put(struct writer *out, u8 value) {
    if (out->offset >= out->limit) {
        out->overflow = 1;
        return;
    }
    out->data[out->offset++] = value;
}

static void put16(struct writer *out, u32 value) {
    put(out, (u8)(value >> 8));
    put(out, (u8)value);
}

static void put_bytes(struct writer *out, const u8 *data, u32 length) {
    for (u32 index = 0; index < length; index++) put(out, data[index]);
}

struct reader {
    const u8 *data;
    u32 offset;
    u32 end;
};

static int take(struct reader *in, u32 length, const u8 **value) {
    if (length > in->end - in->offset) return -1;
    *value = in->data + in->offset;
    in->offset += length;
    return 0;
}

static int take16(struct reader *in, u32 *value) {
    const u8 *bytes;
    if (take(in, 2, &bytes)) return -1;
    *value = ((u32)bytes[0] << 8) | bytes[1];
    return 0;
}

static int take8(struct reader *in, u32 *value) {
    const u8 *bytes;
    if (take(in, 1, &bytes)) return -1;
    *value = bytes[0];
    return 0;
}

static void fail(struct tls_client *client, u8 alert) {
    client->state = TLS_STATE_FAILED;
    client->alert = alert;
}

static void transcript_add(struct tls_client *client, const u8 *message,
                           u32 length) {
    sha256_update(&client->transcript, message, length);
}

static void transcript_hash(const struct tls_client *client,
                            u8 out[TLS_SECRET_SIZE]) {
    // The transcript keeps growing after this point, so the snapshot is taken
    // on a copy. This is the reason the hash context has no internal pointers.
    struct sha256 snapshot = client->transcript;
    sha256_final(&snapshot, out);
}

int tls_client_init(struct tls_client *client, const char *host,
                    u32 host_length, const u8 private_key[TLS_SHARE_SIZE],
                    const u8 random[TLS_RANDOM_SIZE],
                    const u8 *session_id, u32 session_id_length,
                    const struct x509_trust_store *store, u64 now) {
    if (!client || !host || !host_length || host_length > TLS_MAX_HOST)
        return -1;
    if (!private_key || !random || !store) return -1;
    if (session_id_length > TLS_SESSION_ID_SIZE) return -1;
    if (!session_id && session_id_length) return -1;

    u8 *bytes = (u8 *)client;
    for (u32 index = 0; index < sizeof(*client); index++) bytes[index] = 0;

    for (u32 index = 0; index < TLS_SHARE_SIZE; index++)
        client->private_key[index] = private_key[index];
    for (u32 index = 0; index < TLS_RANDOM_SIZE; index++)
        client->random[index] = random[index];
    for (u32 index = 0; index < session_id_length; index++)
        client->session_id[index] = session_id[index];
    client->session_id_length = session_id_length;
    for (u32 index = 0; index < host_length; index++)
        client->host[index] = host[index];
    client->host_length = host_length;
    client->store = store;
    client->now = now;

    if (x25519_base(client->public_key, client->private_key)) return -1;
    sha256_init(&client->transcript);
    tls_key_schedule_init(&client->schedule);
    client->state = TLS_STATE_START;
    return 0;
}

int tls_client_write_hello(struct tls_client *client, u8 *out, u32 *length) {
    if (!client || !out || !length) return -1;
    if (client->state != TLS_STATE_START) return -1;

    struct writer body;
    body.data = out;
    body.offset = 0;
    body.limit = *length;
    body.overflow = 0;

    put(&body, HANDSHAKE_CLIENT_HELLO);
    u32 length_offset = body.offset;
    put(&body, 0);
    put16(&body, 0);

    u32 start = body.offset;
    // legacy_version stays at TLS 1.2 and the real version moves into an
    // extension, which is what lets TLS 1.3 traverse middleboxes at all.
    put16(&body, 0x0303u);
    put_bytes(&body, client->random, TLS_RANDOM_SIZE);
    put(&body, (u8)client->session_id_length);
    put_bytes(&body, client->session_id, client->session_id_length);
    put16(&body, 2);
    put16(&body, CIPHER_AES_128_GCM_SHA256);
    put(&body, 1);
    put(&body, 0);

    u32 extensions_offset = body.offset;
    put16(&body, 0);
    u32 extensions_start = body.offset;

    // server_name: without it a host serving many sites answers with the
    // wrong certificate.
    put16(&body, EXTENSION_SERVER_NAME);
    put16(&body, client->host_length + 5u);
    put16(&body, client->host_length + 3u);
    put(&body, 0);
    put16(&body, client->host_length);
    put_bytes(&body, (const u8 *)client->host, client->host_length);

    put16(&body, EXTENSION_SUPPORTED_VERSIONS);
    put16(&body, 3);
    put(&body, 2);
    put16(&body, 0x0304u);

    put16(&body, EXTENSION_SUPPORTED_GROUPS);
    put16(&body, 4);
    put16(&body, 2);
    put16(&body, GROUP_X25519);

    put16(&body, EXTENSION_SIGNATURE_ALGORITHMS);
    put16(&body, 8);
    put16(&body, 6);
    put16(&body, SIGNATURE_ECDSA_SECP256R1_SHA256);
    put16(&body, SIGNATURE_RSA_PSS_RSAE_SHA256);
    put16(&body, SIGNATURE_RSA_PKCS1_SHA256);

    put16(&body, EXTENSION_KEY_SHARE);
    put16(&body, TLS_SHARE_SIZE + 6u);
    put16(&body, TLS_SHARE_SIZE + 4u);
    put16(&body, GROUP_X25519);
    put16(&body, TLS_SHARE_SIZE);
    put_bytes(&body, client->public_key, TLS_SHARE_SIZE);

    if (body.overflow) return -1;
    u32 extensions_length = body.offset - extensions_start;
    out[extensions_offset] = (u8)(extensions_length >> 8);
    out[extensions_offset + 1u] = (u8)extensions_length;

    u32 body_length = body.offset - start;
    out[length_offset] = (u8)(body_length >> 16);
    out[length_offset + 1u] = (u8)(body_length >> 8);
    out[length_offset + 2u] = (u8)body_length;

    transcript_add(client, out, body.offset);
    *length = body.offset;
    client->state = TLS_STATE_WAIT_SERVER_HELLO;
    return 0;
}

static int read_server_hello(struct tls_client *client, const u8 *body,
                             u32 length) {
    struct reader in;
    in.data = body;
    in.offset = 0;
    in.end = length;

    u32 version;
    const u8 *random;
    u32 session_length;
    const u8 *session;
    u32 cipher;
    u32 compression;
    if (take16(&in, &version) || take(&in, TLS_RANDOM_SIZE, &random) ||
        take8(&in, &session_length) || take(&in, session_length, &session) ||
        take16(&in, &cipher) || take8(&in, &compression)) {
        fail(client, TLS_ALERT_DECODE_ERROR);
        return -1;
    }
    // HelloRetryRequest arrives as a ServerHello carrying this exact random.
    // Only one group is ever offered, so a retry means the server wants
    // something this build does not have.
    static const u8 retry_random[TLS_RANDOM_SIZE] = {
        0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02,
        0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
        0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
    };
    if (crypto_equal(random, retry_random, TLS_RANDOM_SIZE)) {
        fail(client, TLS_ALERT_HANDSHAKE_FAILURE);
        return -1;
    }
    if (cipher != CIPHER_AES_128_GCM_SHA256 || compression) {
        fail(client, TLS_ALERT_ILLEGAL_PARAMETER);
        return -1;
    }
    // The echo has to match what was sent, whatever its length was. A peer
    // that returns something else is not following the compatibility rules
    // and cannot be trusted with the rest of the handshake.
    if (session_length != client->session_id_length ||
        (session_length &&
         !crypto_equal(session, client->session_id, session_length))) {
        fail(client, TLS_ALERT_ILLEGAL_PARAMETER);
        return -1;
    }

    u32 extensions_length;
    if (take16(&in, &extensions_length)) {
        fail(client, TLS_ALERT_DECODE_ERROR);
        return -1;
    }
    struct reader extensions;
    const u8 *extension_data;
    if (take(&in, extensions_length, &extension_data)) {
        fail(client, TLS_ALERT_DECODE_ERROR);
        return -1;
    }
    extensions.data = extension_data;
    extensions.offset = 0;
    extensions.end = extensions_length;

    const u8 *peer_share = 0;
    int negotiated_13 = 0;
    while (extensions.offset < extensions.end) {
        u32 type;
        u32 size;
        const u8 *value;
        if (take16(&extensions, &type) || take16(&extensions, &size) ||
            take(&extensions, size, &value)) {
            fail(client, TLS_ALERT_DECODE_ERROR);
            return -1;
        }
        if (type == EXTENSION_SUPPORTED_VERSIONS) {
            if (size != 2 || value[0] != 0x03u || value[1] != 0x04u) {
                fail(client, TLS_ALERT_PROTOCOL_VERSION);
                return -1;
            }
            negotiated_13 = 1;
        } else if (type == EXTENSION_KEY_SHARE) {
            if (size != TLS_SHARE_SIZE + 4u) {
                fail(client, TLS_ALERT_ILLEGAL_PARAMETER);
                return -1;
            }
            u32 group = ((u32)value[0] << 8) | value[1];
            u32 share_length = ((u32)value[2] << 8) | value[3];
            if (group != GROUP_X25519 || share_length != TLS_SHARE_SIZE) {
                fail(client, TLS_ALERT_ILLEGAL_PARAMETER);
                return -1;
            }
            peer_share = value + 4;
        }
    }
    if (!negotiated_13 || !peer_share) {
        fail(client, TLS_ALERT_PROTOCOL_VERSION);
        return -1;
    }

    u8 shared[TLS_SHARE_SIZE];
    // A small-order peer share collapses the secret to zero, which x25519
    // reports rather than returning.
    if (x25519(shared, client->private_key, peer_share)) {
        fail(client, TLS_ALERT_ILLEGAL_PARAMETER);
        return -1;
    }
    u8 hash[TLS_SECRET_SIZE];
    transcript_hash(client, hash);
    int derived = tls_key_schedule_handshake(&client->schedule, shared,
                                             TLS_SHARE_SIZE, hash);
    crypto_zero(shared, sizeof(shared));
    if (derived) {
        fail(client, TLS_ALERT_HANDSHAKE_FAILURE);
        return -1;
    }
    if (tls_record_init(&client->reader,
                        client->schedule.server_handshake_traffic) ||
        tls_record_init(&client->writer,
                        client->schedule.client_handshake_traffic)) {
        fail(client, TLS_ALERT_HANDSHAKE_FAILURE);
        return -1;
    }
    client->state = TLS_STATE_WAIT_ENCRYPTED_EXTENSIONS;
    return 0;
}

static int read_certificate(struct tls_client *client, const u8 *body,
                            u32 length) {
    struct reader in;
    in.data = body;
    in.offset = 0;
    in.end = length;
    u32 context_length;
    const u8 *context;
    if (take8(&in, &context_length) || take(&in, context_length, &context)) {
        fail(client, TLS_ALERT_DECODE_ERROR);
        return -1;
    }
    // certificate_list is a 24 bit length: one high byte and then two more.
    // Reading it as two 16 bit halves consumes an extra byte and shifts every
    // field after it.
    u32 list_high;
    u32 list_low;
    const u8 *list;
    if (take8(&in, &list_high) || take16(&in, &list_low)) {
        fail(client, TLS_ALERT_DECODE_ERROR);
        return -1;
    }
    u32 list_length = (list_high << 16) | list_low;
    if (take(&in, list_length, &list)) {
        fail(client, TLS_ALERT_DECODE_ERROR);
        return -1;
    }

    const u8 *chain[X509_MAX_CHAIN];
    u32 lengths[X509_MAX_CHAIN];
    u32 count = 0;
    struct reader entries;
    entries.data = list;
    entries.offset = 0;
    entries.end = list_length;
    while (entries.offset < entries.end) {
        u32 high;
        u32 middle;
        if (take8(&entries, &high) || take16(&entries, &middle)) {
            fail(client, TLS_ALERT_DECODE_ERROR);
            return -1;
        }
        u32 certificate_length = (high << 16) | middle;
        const u8 *certificate;
        if (take(&entries, certificate_length, &certificate)) {
            fail(client, TLS_ALERT_DECODE_ERROR);
            return -1;
        }
        u32 extension_length;
        const u8 *extensions;
        if (take16(&entries, &extension_length) ||
            take(&entries, extension_length, &extensions)) {
            fail(client, TLS_ALERT_DECODE_ERROR);
            return -1;
        }
        if (count < X509_MAX_CHAIN) {
            chain[count] = certificate;
            lengths[count] = certificate_length;
            count++;
        }
    }
    if (!count) {
        fail(client, TLS_ALERT_BAD_CERTIFICATE);
        return -1;
    }

    if (x509_verify_chain(chain, lengths, count, client->store, client->host,
                          client->host_length, client->now)) {
        fail(client, TLS_ALERT_BAD_CERTIFICATE);
        return -1;
    }

    // The leaf key is needed after this buffer is gone, so it is copied out.
    static struct x509_certificate leaf;
    if (x509_parse(&leaf, chain[0], lengths[0])) {
        fail(client, TLS_ALERT_BAD_CERTIFICATE);
        return -1;
    }
    client->certificate_key_algorithm = (u8)leaf.key_algorithm;
    if (leaf.key_algorithm == X509_KEY_EC_P256) {
        for (u32 index = 0; index < sizeof(client->certificate_point); index++)
            client->certificate_point[index] = leaf.point[index];
    } else if (leaf.key_algorithm == X509_KEY_RSA) {
        if (lengths[0] > sizeof(client->chain)) {
            fail(client, TLS_ALERT_BAD_CERTIFICATE);
            return -1;
        }
        for (u32 index = 0; index < lengths[0]; index++)
            client->chain[index] = chain[0][index];
        client->chain_length = lengths[0];
        static struct x509_certificate copied;
        if (x509_parse(&copied, client->chain, client->chain_length)) {
            fail(client, TLS_ALERT_BAD_CERTIFICATE);
            return -1;
        }
        client->certificate_modulus = copied.modulus;
        client->certificate_modulus_length = copied.modulus_length;
        client->certificate_exponent = copied.exponent;
    } else {
        fail(client, TLS_ALERT_BAD_CERTIFICATE);
        return -1;
    }
    client->state = TLS_STATE_WAIT_CERTIFICATE_VERIFY;
    return 0;
}

static int read_certificate_verify(struct tls_client *client, const u8 *body,
                                   u32 length, const u8 *transcript_before) {
    struct reader in;
    in.data = body;
    in.offset = 0;
    in.end = length;
    u32 algorithm;
    u32 signature_length;
    const u8 *signature;
    if (take16(&in, &algorithm) || take16(&in, &signature_length) ||
        take(&in, signature_length, &signature) || in.offset != in.end) {
        fail(client, TLS_ALERT_DECODE_ERROR);
        return -1;
    }

    u8 signed_data[sizeof(certificate_verify_context) + TLS_SECRET_SIZE];
    for (u32 index = 0; index < sizeof(certificate_verify_context); index++)
        signed_data[index] = certificate_verify_context[index];
    for (u32 index = 0; index < TLS_SECRET_SIZE; index++)
        signed_data[sizeof(certificate_verify_context) + index] =
            transcript_before[index];
    u8 digest[TLS_SECRET_SIZE];
    sha256_digest(signed_data, sizeof(signed_data), digest);

    int verified = -1;
    if (algorithm == SIGNATURE_ECDSA_SECP256R1_SHA256) {
        if (client->certificate_key_algorithm != X509_KEY_EC_P256) {
            fail(client, TLS_ALERT_ILLEGAL_PARAMETER);
            return -1;
        }
        struct der_reader reader;
        struct der_reader sequence;
        const u8 *value;
        u32 value_length;
        u8 r[32];
        u8 s[32];
        if (der_init(&reader, signature, signature_length) ||
            der_read_nested(&reader, DER_TAG_SEQUENCE, &sequence)) {
            fail(client, TLS_ALERT_DECODE_ERROR);
            return -1;
        }
        for (u32 half = 0; half < 2; half++) {
            u8 *out = half ? s : r;
            if (der_read_unsigned(&sequence, &value, &value_length) ||
                !value_length || value_length > 32u) {
                fail(client, TLS_ALERT_DECODE_ERROR);
                return -1;
            }
            for (u32 index = 0; index < 32u; index++) out[index] = 0;
            for (u32 index = 0; index < value_length; index++)
                out[32u - value_length + index] = value[index];
        }
        verified = p256_verify(client->certificate_point, digest, r, s);
    } else if (algorithm == SIGNATURE_RSA_PSS_RSAE_SHA256 ||
               algorithm == SIGNATURE_RSA_PKCS1_SHA256) {
        if (client->certificate_key_algorithm != X509_KEY_RSA) {
            fail(client, TLS_ALERT_ILLEGAL_PARAMETER);
            return -1;
        }
        static struct rsa_public_key key;
        if (rsa_public_key_init(&key, client->certificate_modulus,
                                client->certificate_modulus_length,
                                client->certificate_exponent)) {
            fail(client, TLS_ALERT_BAD_CERTIFICATE);
            return -1;
        }
        verified = algorithm == SIGNATURE_RSA_PSS_RSAE_SHA256
            ? rsa_pss_verify_sha256(&key, digest, signature, signature_length)
            : rsa_pkcs1_verify_sha256(&key, digest, signature,
                                      signature_length);
    } else {
        fail(client, TLS_ALERT_ILLEGAL_PARAMETER);
        return -1;
    }

    if (verified) {
        fail(client, TLS_ALERT_DECRYPT_ERROR);
        return -1;
    }
    client->state = TLS_STATE_WAIT_FINISHED;
    return 0;
}

static int finished_mac(const u8 secret[TLS_SECRET_SIZE],
                        const u8 transcript[TLS_SECRET_SIZE],
                        u8 out[TLS_SECRET_SIZE]) {
    u8 finished_key[TLS_SECRET_SIZE];
    if (tls_hkdf_expand_label(secret, "finished", 0, 0, finished_key,
                              TLS_SECRET_SIZE))
        return -1;
    hmac_sha256_digest(finished_key, TLS_SECRET_SIZE, transcript,
                       TLS_SECRET_SIZE, out);
    crypto_zero(finished_key, sizeof(finished_key));
    return 0;
}

static int read_finished(struct tls_client *client, const u8 *body,
                         u32 length, const u8 *transcript_before) {
    if (length != TLS_SECRET_SIZE) {
        fail(client, TLS_ALERT_DECODE_ERROR);
        return -1;
    }
    u8 expected[TLS_SECRET_SIZE];
    if (finished_mac(client->schedule.server_handshake_traffic,
                     transcript_before, expected)) {
        fail(client, TLS_ALERT_HANDSHAKE_FAILURE);
        return -1;
    }
    // A mismatch means the transcripts diverged, which is exactly what an
    // injected or altered handshake message produces.
    int matched = crypto_equal(expected, body, TLS_SECRET_SIZE);
    crypto_zero(expected, sizeof(expected));
    if (!matched) {
        fail(client, TLS_ALERT_DECRYPT_ERROR);
        return -1;
    }
    return 0;
}

int tls_client_read_message(struct tls_client *client, const u8 *message,
                            u32 length) {
    if (!client || !message) return -1;
    if (client->state == TLS_STATE_FAILED) return -1;
    if (length < 4u || length > TLS_MAX_HANDSHAKE_MESSAGE) {
        fail(client, TLS_ALERT_DECODE_ERROR);
        return -1;
    }
    u8 type = message[0];
    u32 body_length = ((u32)message[1] << 16) | ((u32)message[2] << 8) |
        message[3];
    if (body_length + 4u != length) {
        fail(client, TLS_ALERT_DECODE_ERROR);
        return -1;
    }
    const u8 *body = message + 4;

    // Both CertificateVerify and Finished are checked against the transcript
    // as it stood before them, so the snapshot is taken first.
    u8 before[TLS_SECRET_SIZE];
    transcript_hash(client, before);

    switch (client->state) {
    case TLS_STATE_WAIT_SERVER_HELLO:
        if (type != HANDSHAKE_SERVER_HELLO) {
            fail(client, TLS_ALERT_HANDSHAKE_FAILURE);
            return -1;
        }
        transcript_add(client, message, length);
        return read_server_hello(client, body, body_length);
    case TLS_STATE_WAIT_ENCRYPTED_EXTENSIONS:
        if (type != HANDSHAKE_ENCRYPTED_EXTENSIONS) {
            fail(client, TLS_ALERT_HANDSHAKE_FAILURE);
            return -1;
        }
        transcript_add(client, message, length);
        client->state = TLS_STATE_WAIT_CERTIFICATE;
        return 0;
    case TLS_STATE_WAIT_CERTIFICATE:
        if (type != HANDSHAKE_CERTIFICATE) {
            fail(client, TLS_ALERT_HANDSHAKE_FAILURE);
            return -1;
        }
        transcript_add(client, message, length);
        return read_certificate(client, body, body_length);
    case TLS_STATE_WAIT_CERTIFICATE_VERIFY:
        if (type != HANDSHAKE_CERTIFICATE_VERIFY) {
            fail(client, TLS_ALERT_HANDSHAKE_FAILURE);
            return -1;
        }
        if (read_certificate_verify(client, body, body_length, before))
            return -1;
        transcript_add(client, message, length);
        return 0;
    case TLS_STATE_WAIT_FINISHED:
        if (type != HANDSHAKE_FINISHED) {
            fail(client, TLS_ALERT_HANDSHAKE_FAILURE);
            return -1;
        }
        if (read_finished(client, body, body_length, before)) return -1;
        transcript_add(client, message, length);
        client->state = TLS_STATE_CONNECTED;
        return 0;
    case TLS_STATE_CONNECTED:
        // A ticket is the only thing that legitimately arrives here, and this
        // build has no session resumption to spend it on.
        if (type == HANDSHAKE_NEW_SESSION_TICKET) return 0;
        fail(client, TLS_ALERT_HANDSHAKE_FAILURE);
        return -1;
    default:
        fail(client, TLS_ALERT_HANDSHAKE_FAILURE);
        return -1;
    }
}

int tls_client_write_finished(struct tls_client *client, u8 *out,
                              u32 *length) {
    if (!client || !out || !length) return -1;
    if (client->state != TLS_STATE_CONNECTED) return -1;
    if (*length < 4u + TLS_SECRET_SIZE) return -1;
    u8 transcript[TLS_SECRET_SIZE];
    transcript_hash(client, transcript);
    u8 mac[TLS_SECRET_SIZE];
    if (finished_mac(client->schedule.client_handshake_traffic, transcript,
                     mac))
        return -1;
    out[0] = HANDSHAKE_FINISHED;
    out[1] = 0;
    out[2] = 0;
    out[3] = (u8)TLS_SECRET_SIZE;
    for (u32 index = 0; index < TLS_SECRET_SIZE; index++)
        out[4u + index] = mac[index];
    crypto_zero(mac, sizeof(mac));
    *length = 4u + TLS_SECRET_SIZE;
    return 0;
}

int tls_client_activate_application_keys(struct tls_client *client) {
    if (!client || client->state != TLS_STATE_CONNECTED) return -1;
    // Application keys derive from the transcript through the server Finished,
    // not through the client one, so this is safe to call after writing ours.
    u8 transcript[TLS_SECRET_SIZE];
    struct sha256 snapshot = client->transcript;
    sha256_final(&snapshot, transcript);
    if (tls_key_schedule_application(&client->schedule, transcript)) return -1;
    if (tls_record_init(&client->reader,
                        client->schedule.server_application_traffic))
        return -1;
    return tls_record_init(&client->writer,
                           client->schedule.client_application_traffic);
}

void tls_client_clear(struct tls_client *client) {
    if (!client) return;
    crypto_zero(client, sizeof(*client));
}

// Pulls whole handshake messages out of the reassembled plaintext. A record
// may carry several messages, or a fraction of one.
static int drain_messages(struct tls_client *client) {
    u32 offset = 0;
    while (client->plain_length - offset >= 4u) {
        const u8 *header = client->plain + offset;
        u32 body = ((u32)header[1] << 16) | ((u32)header[2] << 8) | header[3];
        u32 total = body + 4u;
        if (total > TLS_MAX_HANDSHAKE_MESSAGE) {
            fail(client, TLS_ALERT_DECODE_ERROR);
            return -1;
        }
        if (client->plain_length - offset < total) break;
        if (tls_client_read_message(client, header, total)) return -1;
        offset += total;
    }
    // Keep the tail that belongs to a message still in flight.
    u32 remaining = client->plain_length - offset;
    for (u32 index = 0; index < remaining; index++)
        client->plain[index] = client->plain[offset + index];
    client->plain_length = remaining;
    return 0;
}

static int accept_record(struct tls_client *client, const u8 *record,
                         u32 length) {
    u8 type = record[0];
    const u8 *body = record + TLS_RECORD_HEADER_SIZE;
    u32 body_length = length - TLS_RECORD_HEADER_SIZE;

    // A middlebox-compatibility ChangeCipherSpec carries no meaning here and
    // must not disturb the transcript.
    if (type == TLS_CONTENT_CHANGE_CIPHER_SPEC) return 0;

    if (client->state == TLS_STATE_WAIT_SERVER_HELLO) {
        if (type != TLS_CONTENT_HANDSHAKE) {
            fail(client, TLS_ALERT_HANDSHAKE_FAILURE);
            return -1;
        }
        if (body_length > sizeof(client->plain) - client->plain_length) {
            fail(client, TLS_ALERT_DECODE_ERROR);
            return -1;
        }
        for (u32 index = 0; index < body_length; index++)
            client->plain[client->plain_length + index] = body[index];
        client->plain_length += body_length;
        return drain_messages(client);
    }

    if (type != TLS_CONTENT_APPLICATION_DATA) {
        fail(client, TLS_ALERT_HANDSHAKE_FAILURE);
        return -1;
    }
    static u8 opened[TLS_RECORD_MAX_CIPHERTEXT];
    u32 opened_length = 0;
    u8 inner_type = 0;
    if (tls_record_open(&client->reader, record, length, opened,
                        &opened_length, &inner_type)) {
        fail(client, TLS_ALERT_DECRYPT_ERROR);
        return -1;
    }
    if (inner_type == TLS_CONTENT_ALERT) {
        fail(client, TLS_ALERT_HANDSHAKE_FAILURE);
        return -1;
    }
    if (inner_type != TLS_CONTENT_HANDSHAKE) return 0;
    if (opened_length > sizeof(client->plain) - client->plain_length) {
        fail(client, TLS_ALERT_DECODE_ERROR);
        return -1;
    }
    for (u32 index = 0; index < opened_length; index++)
        client->plain[client->plain_length + index] = opened[index];
    client->plain_length += opened_length;
    return drain_messages(client);
}

int tls_client_feed(struct tls_client *client, const u8 *data, u32 length) {
    if (!client || (!data && length)) return -1;
    if (client->state == TLS_STATE_FAILED) return -1;
    u32 offset = 0;
    while (offset < length) {
        u32 space = sizeof(client->record) - client->record_buffered;
        if (!space) {
            fail(client, TLS_ALERT_DECODE_ERROR);
            return -1;
        }
        u32 take = length - offset;
        if (take > space) take = space;
        for (u32 index = 0; index < take; index++)
            client->record[client->record_buffered + index] = data[offset + index];
        client->record_buffered += take;
        offset += take;

        while (client->record_buffered >= TLS_RECORD_HEADER_SIZE) {
            u32 body = ((u32)client->record[3] << 8) | client->record[4];
            if (body > TLS_RECORD_MAX_CIPHERTEXT) {
                fail(client, TLS_ALERT_DECODE_ERROR);
                return -1;
            }
            u32 total = TLS_RECORD_HEADER_SIZE + body;
            if (client->record_buffered < total) break;
            if (accept_record(client, client->record, total)) return -1;
            u32 rest = client->record_buffered - total;
            for (u32 index = 0; index < rest; index++)
                client->record[index] = client->record[total + index];
            client->record_buffered = rest;
        }
    }
    return 0;
}

int tls_client_seal(struct tls_client *client, u8 content_type,
                    const u8 *payload, u32 length, u8 *out, u32 *out_length) {
    if (!client || client->state == TLS_STATE_FAILED) return -1;
    return tls_record_seal(&client->writer, content_type, payload, length, out,
                           out_length);
}

int tls_client_open(struct tls_client *client, const u8 *record,
                    u32 record_length, u8 *out, u32 *out_length,
                    u8 *content_type) {
    if (!client || client->state != TLS_STATE_CONNECTED) return -1;
    return tls_record_open(&client->reader, record, record_length, out,
                           out_length, content_type);
}
