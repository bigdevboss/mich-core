#ifndef MICH_NET_TLS_HANDSHAKE_H
#define MICH_NET_TLS_HANDSHAKE_H

#include "types.h"
#include "tls_keys.h"
#include "tls_record.h"
#include "x509_chain.h"

#define TLS_MAX_HOST 255u
#define TLS_RANDOM_SIZE 32u
#define TLS_SESSION_ID_SIZE 32u
#define TLS_SHARE_SIZE 32u

// A server sends its certificate chain inside one handshake message, and a
// handshake message may arrive spread over several records. This bounds both
// the reassembly buffer and the chain.
#define TLS_MAX_HANDSHAKE_MESSAGE 16384u
// Only the leaf is kept after the record that carried it is gone, and a leaf
// larger than this does not appear in practice.
#define TLS_MAX_CERTIFICATE_COPY 4096u

#define TLS_STATE_START 0u
#define TLS_STATE_WAIT_SERVER_HELLO 1u
#define TLS_STATE_WAIT_ENCRYPTED_EXTENSIONS 2u
#define TLS_STATE_WAIT_CERTIFICATE 3u
#define TLS_STATE_WAIT_CERTIFICATE_VERIFY 4u
#define TLS_STATE_WAIT_FINISHED 5u
#define TLS_STATE_CONNECTED 6u
#define TLS_STATE_FAILED 7u

#define TLS_ALERT_LEVEL_FATAL 2u
#define TLS_ALERT_HANDSHAKE_FAILURE 40u
#define TLS_ALERT_BAD_CERTIFICATE 42u
#define TLS_ALERT_DECRYPT_ERROR 51u
#define TLS_ALERT_PROTOCOL_VERSION 70u
#define TLS_ALERT_ILLEGAL_PARAMETER 47u
#define TLS_ALERT_DECODE_ERROR 50u

struct tls_client {
    u32 state;
    u8 alert;

    u8 private_key[TLS_SHARE_SIZE];
    u8 public_key[TLS_SHARE_SIZE];
    u8 random[TLS_RANDOM_SIZE];
    u8 session_id[TLS_SESSION_ID_SIZE];
    u32 session_id_length;

    struct sha256 transcript;
    struct tls_key_schedule schedule;
    struct tls_record_context reader;
    struct tls_record_context writer;

    char host[TLS_MAX_HOST];
    u32 host_length;
    u64 now;

    const struct x509_trust_store *store;

    u8 certificate_key_algorithm;
    u8 certificate_point[65];
    const u8 *certificate_modulus;
    u32 certificate_modulus_length;
    u32 certificate_exponent;
    // The leaf lives in the caller's buffer, so the chain message has to be
    // copied here: the signature over it is checked after the record that
    // carried it is gone.
    u8 chain[TLS_MAX_CERTIFICATE_COPY];
    u32 chain_length;


    // Two levels of reassembly. TCP hands over arbitrary byte runs, a record
    // may span several of them, and a handshake message may span several
    // records: Certificate alone does not fit in one.
    u8 record[TLS_RECORD_HEADER_SIZE + TLS_RECORD_MAX_CIPHERTEXT];
    u32 record_buffered;
    u8 plain[TLS_MAX_HANDSHAKE_MESSAGE];
    u32 plain_length;
};

// private_key and random come from the caller: this file has no entropy source
// of its own, and a test needs to supply the values a published trace used.
int tls_client_init(struct tls_client *client, const char *host,
                    u32 host_length, const u8 private_key[TLS_SHARE_SIZE],
                    const u8 random[TLS_RANDOM_SIZE],
                    const u8 *session_id, u32 session_id_length,
                    const struct x509_trust_store *store, u64 now);

// Builds ClientHello and folds it into the transcript.
int tls_client_write_hello(struct tls_client *client, u8 *out, u32 *length);

// Feeds one handshake message, without its record framing. Advances the state
// machine; on failure sets client->alert to the code a peer should be told.
int tls_client_read_message(struct tls_client *client, const u8 *message,
                            u32 length);

// Builds the client Finished once the server side has been accepted.
int tls_client_write_finished(struct tls_client *client, u8 *out,
                              u32 *length);

// Installs the application traffic keys. Called after the client Finished has
// been written, because that message is still protected by handshake keys.
int tls_client_activate_application_keys(struct tls_client *client);

// Feeds bytes straight off the socket. Reassembles records, decrypts the ones
// that are protected, and hands whole handshake messages to the state machine.
// Returns 0 while progress is possible, -1 on a protocol failure.
int tls_client_feed(struct tls_client *client, const u8 *data, u32 length);

// Wraps payload into a record with the current write keys.
int tls_client_seal(struct tls_client *client, u8 content_type,
                    const u8 *payload, u32 length, u8 *out, u32 *out_length);

// Unwraps one application-data record already reassembled by tls_client_feed.
int tls_client_open(struct tls_client *client, const u8 *record,
                    u32 record_length, u8 *out, u32 *out_length,
                    u8 *content_type);

void tls_client_clear(struct tls_client *client);

#endif
