#include "types.h"
#include "tls_handshake.h"
#include "crypto.h"
#include "tests64.h"

// The ClientHello and ServerHello of the handshake published in RFC 8448,
// together with the client private key that trace used. Feeding the recorded
// ServerHello and arriving at the recorded secrets proves the key exchange,
// the transcript and the schedule all agree with the specification.

static const u8 rfc_client_private[32] = {
    0x49, 0xAF, 0x42, 0xBA, 0x7F, 0x79, 0x94, 0x85, 0x2D, 0x71, 0x3E, 0xF2,
    0x78, 0x4B, 0xCB, 0xCA, 0xA7, 0x91, 0x1D, 0xE2, 0x6A, 0xDC, 0x56, 0x42,
    0xCB, 0x63, 0x45, 0x40, 0xE7, 0xEA, 0x50, 0x05,
};

static const u8 rfc_client_public[32] = {
    0x99, 0x38, 0x1D, 0xE5, 0x60, 0xE4, 0xBD, 0x43, 0xD2, 0x3D, 0x8E, 0x43,
    0x5A, 0x7D, 0xBA, 0xFE, 0xB3, 0xC0, 0x6E, 0x51, 0xC1, 0x3C, 0xAE, 0x4D,
    0x54, 0x13, 0x69, 0x1E, 0x52, 0x9A, 0xAF, 0x2C,
};

static const u8 rfc_client_hello[196] = {
    0x01, 0x00, 0x00, 0xC0, 0x03, 0x03, 0xCB, 0x34, 0xEC, 0xB1, 0xE7, 0x81,
    0x63, 0xBA, 0x1C, 0x38, 0xC6, 0xDA, 0xCB, 0x19, 0x6A, 0x6D, 0xFF, 0xA2,
    0x1A, 0x8D, 0x99, 0x12, 0xEC, 0x18, 0xA2, 0xEF, 0x62, 0x83, 0x02, 0x4D,
    0xEC, 0xE7, 0x00, 0x00, 0x06, 0x13, 0x01, 0x13, 0x03, 0x13, 0x02, 0x01,
    0x00, 0x00, 0x91, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x09, 0x00, 0x00, 0x06,
    0x73, 0x65, 0x72, 0x76, 0x65, 0x72, 0xFF, 0x01, 0x00, 0x01, 0x00, 0x00,
    0x0A, 0x00, 0x14, 0x00, 0x12, 0x00, 0x1D, 0x00, 0x17, 0x00, 0x18, 0x00,
    0x19, 0x01, 0x00, 0x01, 0x01, 0x01, 0x02, 0x01, 0x03, 0x01, 0x04, 0x00,
    0x23, 0x00, 0x00, 0x00, 0x33, 0x00, 0x26, 0x00, 0x24, 0x00, 0x1D, 0x00,
    0x20, 0x99, 0x38, 0x1D, 0xE5, 0x60, 0xE4, 0xBD, 0x43, 0xD2, 0x3D, 0x8E,
    0x43, 0x5A, 0x7D, 0xBA, 0xFE, 0xB3, 0xC0, 0x6E, 0x51, 0xC1, 0x3C, 0xAE,
    0x4D, 0x54, 0x13, 0x69, 0x1E, 0x52, 0x9A, 0xAF, 0x2C, 0x00, 0x2B, 0x00,
    0x03, 0x02, 0x03, 0x04, 0x00, 0x0D, 0x00, 0x20, 0x00, 0x1E, 0x04, 0x03,
    0x05, 0x03, 0x06, 0x03, 0x02, 0x03, 0x08, 0x04, 0x08, 0x05, 0x08, 0x06,
    0x04, 0x01, 0x05, 0x01, 0x06, 0x01, 0x02, 0x01, 0x04, 0x02, 0x05, 0x02,
    0x06, 0x02, 0x02, 0x02, 0x00, 0x2D, 0x00, 0x02, 0x01, 0x01, 0x00, 0x1C,
    0x00, 0x02, 0x40, 0x01,
};

static const u8 rfc_server_hello[90] = {
    0x02, 0x00, 0x00, 0x56, 0x03, 0x03, 0xA6, 0xAF, 0x06, 0xA4, 0x12, 0x18,
    0x60, 0xDC, 0x5E, 0x6E, 0x60, 0x24, 0x9C, 0xD3, 0x4C, 0x95, 0x93, 0x0C,
    0x8A, 0xC5, 0xCB, 0x14, 0x34, 0xDA, 0xC1, 0x55, 0x77, 0x2E, 0xD3, 0xE2,
    0x69, 0x28, 0x00, 0x13, 0x01, 0x00, 0x00, 0x2E, 0x00, 0x33, 0x00, 0x24,
    0x00, 0x1D, 0x00, 0x20, 0xC9, 0x82, 0x88, 0x76, 0x11, 0x20, 0x95, 0xFE,
    0x66, 0x76, 0x2B, 0xDB, 0xF7, 0xC6, 0x72, 0xE1, 0x56, 0xD6, 0xCC, 0x25,
    0x3B, 0x83, 0x3D, 0xF1, 0xDD, 0x69, 0xB1, 0xB0, 0x4E, 0x75, 0x1F, 0x0F,
    0x00, 0x2B, 0x00, 0x02, 0x03, 0x04,
};

static const u8 rfc_handshake_secret[32] = {
    0x1D, 0xC8, 0x26, 0xE9, 0x36, 0x06, 0xAA, 0x6F, 0xDC, 0x0A, 0xAD, 0xC1,
    0x2F, 0x74, 0x1B, 0x01, 0x04, 0x6A, 0xA6, 0xB9, 0x9F, 0x69, 0x1E, 0xD2,
    0x21, 0xA9, 0xF0, 0xCA, 0x04, 0x3F, 0xBE, 0xAC,
};

static const u8 rfc_client_handshake[32] = {
    0xB3, 0xED, 0xDB, 0x12, 0x6E, 0x06, 0x7F, 0x35, 0xA7, 0x80, 0xB3, 0xAB,
    0xF4, 0x5E, 0x2D, 0x8F, 0x3B, 0x1A, 0x95, 0x07, 0x38, 0xF5, 0x2E, 0x96,
    0x00, 0x74, 0x6A, 0x0E, 0x27, 0xA5, 0x5A, 0x21,
};

static const u8 rfc_server_handshake[32] = {
    0xB6, 0x7B, 0x7D, 0x69, 0x0C, 0xC1, 0x6C, 0x4E, 0x75, 0xE5, 0x42, 0x13,
    0xCB, 0x2D, 0x37, 0xB4, 0xE9, 0xC9, 0x12, 0xBC, 0xDE, 0xD9, 0x10, 0x5D,
    0x42, 0xBE, 0xFD, 0x59, 0xD3, 0x91, 0xAD, 0x38,
};


// ClientHello layout: type(1) length(3) version(2) random(32) then the
// session id, which this trace leaves empty.
#define RFC_RANDOM (rfc_client_hello + 6u)
#define RFC_SESSION_ID (rfc_client_hello + 39u)
#define RFC_SESSION_ID_LENGTH ((u32)rfc_client_hello[38])

static int start_from_trace(struct tls_client *client,
                            const struct x509_trust_store *store) {
    if (tls_client_init(client, "server", 6u, rfc_client_private, RFC_RANDOM,
                        RFC_SESSION_ID, RFC_SESSION_ID_LENGTH, store,
                        1790240000ull))
        return -1;
    // The trace's ClientHello carries extensions this build does not send, so
    // it is folded into the transcript directly rather than rebuilt.
    sha256_update(&client->transcript, rfc_client_hello,
                  sizeof(rfc_client_hello));
    client->state = TLS_STATE_WAIT_SERVER_HELLO;
    return 0;
}

int test_tls_handshake64(void) {
    const struct x509_trust_store *store = x509_builtin_trust_store();
    // Each client carries the reassembly and chain buffers, so four of them on
    // the stack would overrun the 128 KiB the kernel gives this path.
    static struct tls_client client;

    int valid = !start_from_trace(&client, store) &&
        crypto_equal(client.public_key, rfc_client_public, TLS_SHARE_SIZE);

    valid = valid && !tls_client_read_message(&client, rfc_server_hello,
                                              sizeof(rfc_server_hello)) &&
        crypto_equal(client.schedule.handshake_secret, rfc_handshake_secret,
                     TLS_SECRET_SIZE) &&
        crypto_equal(client.schedule.client_handshake_traffic,
                     rfc_client_handshake, TLS_SECRET_SIZE) &&
        crypto_equal(client.schedule.server_handshake_traffic,
                     rfc_server_handshake, TLS_SECRET_SIZE) &&
        client.state == TLS_STATE_WAIT_ENCRYPTED_EXTENSIONS;

    // Everything a hostile or broken server can put in ServerHello.
    static u8 broken[256];
    static struct tls_client rejecting;
    u32 offsets[3];
    u8 masks[3];
    // cipher suite, compression, and the handshake type itself
    offsets[0] = 4u + 2u + TLS_RANDOM_SIZE + 1u;
    masks[0] = 0xFFu;
    offsets[1] = 4u + 2u + TLS_RANDOM_SIZE + 1u + 2u;
    masks[1] = 0x01u;
    offsets[2] = 0u;
    masks[2] = 0x10u;
    for (u32 index = 0; index < 3u; index++) {
        for (u32 byte = 0; byte < sizeof(rfc_server_hello); byte++)
            broken[byte] = rfc_server_hello[byte];
        broken[offsets[index]] ^= masks[index];
        valid = valid && !start_from_trace(&rejecting, store) &&
            tls_client_read_message(&rejecting, broken,
                                    sizeof(rfc_server_hello)) == -1 &&
            rejecting.state == TLS_STATE_FAILED && rejecting.alert;
    }

    // A ServerHello whose session id echo does not match what was sent.
    static const u8 other_session[TLS_SESSION_ID_SIZE] = { 0xA5 };
    valid = valid &&
        !tls_client_init(&rejecting, "server", 6u, rfc_client_private,
                         RFC_RANDOM, other_session, TLS_SESSION_ID_SIZE, store,
                         1790240000ull);
    sha256_update(&rejecting.transcript, rfc_client_hello,
                  sizeof(rfc_client_hello));
    rejecting.state = TLS_STATE_WAIT_SERVER_HELLO;
    valid = valid && tls_client_read_message(&rejecting, rfc_server_hello,
                                             sizeof(rfc_server_hello)) == -1;

    // HelloRetryRequest is a ServerHello carrying a fixed random. Only one
    // group is ever offered, so a retry cannot be satisfied and is refused
    // with an alert rather than ignored.
    static const u8 retry_random[TLS_RANDOM_SIZE] = {
        0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02,
        0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E,
        0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
    };
    for (u32 byte = 0; byte < sizeof(rfc_server_hello); byte++)
        broken[byte] = rfc_server_hello[byte];
    for (u32 index = 0; index < TLS_RANDOM_SIZE; index++)
        broken[6u + index] = retry_random[index];
    valid = valid && !start_from_trace(&rejecting, store) &&
        tls_client_read_message(&rejecting, broken,
                                sizeof(rfc_server_hello)) == -1 &&
        rejecting.alert == TLS_ALERT_HANDSHAKE_FAILURE;

    // Messages out of order, truncated, or claiming a length they do not have.
    valid = valid && !start_from_trace(&rejecting, store) &&
        tls_client_read_message(&rejecting, rfc_server_hello, 3u) == -1;
    valid = valid && !start_from_trace(&rejecting, store) &&
        tls_client_read_message(&rejecting, rfc_server_hello,
                                sizeof(rfc_server_hello) - 1u) == -1;

    // The ClientHello this build produces.
    static struct tls_client writer;
    static u8 hello[1024];
    u32 hello_length = sizeof(hello);
    valid = valid &&
        !tls_client_init(&writer, "example.com", 11u, rfc_client_private,
                         RFC_RANDOM, other_session, TLS_SESSION_ID_SIZE, store,
                         1790240000ull) &&
        !tls_client_write_hello(&writer, hello, &hello_length) &&
        hello[0] == 1u &&
        (((u32)hello[1] << 16) | ((u32)hello[2] << 8) | hello[3]) + 4u ==
            hello_length &&
        // legacy_version stays 1.2 on the wire; the real one is an extension.
        hello[4] == 0x03u && hello[5] == 0x03u &&
        writer.state == TLS_STATE_WAIT_SERVER_HELLO;

    // The server name has to be in there, or a shared host answers with the
    // wrong certificate.
    int found_host = 0;
    for (u32 index = 0; index + 11u <= hello_length; index++)
        if (crypto_equal(hello + index, "example.com", 11u)) found_host = 1;
    valid = valid && found_host;

    // A second hello from the same client is a protocol error.
    hello_length = sizeof(hello);
    valid = valid &&
        tls_client_write_hello(&writer, hello, &hello_length) == -1;

    // Too small a buffer must be refused rather than overrun.
    hello_length = 16u;
    static struct tls_client cramped;
    valid = valid &&
        !tls_client_init(&cramped, "example.com", 11u, rfc_client_private,
                         RFC_RANDOM, other_session, TLS_SESSION_ID_SIZE, store,
                         1790240000ull) &&
        tls_client_write_hello(&cramped, hello, &hello_length) == -1;

    valid = valid &&
        tls_client_init(&cramped, 0, 11u, rfc_client_private, RFC_RANDOM,
                        other_session, TLS_SESSION_ID_SIZE, store, 0) == -1 &&
        tls_client_init(&cramped, "a", 0u, rfc_client_private, RFC_RANDOM,
                        other_session, TLS_SESSION_ID_SIZE, store, 0) == -1 &&
        tls_client_init(&cramped, "a", 1u, rfc_client_private, RFC_RANDOM,
                        other_session, TLS_SESSION_ID_SIZE + 1u, store,
                        0) == -1;

    tls_client_clear(&client);
    valid = valid && !client.private_key[0] && !client.state;
    return valid ? 0 : -1;
}
