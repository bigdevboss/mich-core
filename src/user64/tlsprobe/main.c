#include <mich/syscall.h>
#include <mich/socket.h>
#include <mich/event.h>
#include <mich/timer.h>
#include <tls_handshake.h>
#include <crypto.h>
#include "test_anchor.h"

// Declared here rather than pulled in through a POSIX header: this module is
// freestanding and only needs the one entry point.
extern long getrandom(void *buffer, unsigned long length, unsigned int flags);

// Drives a complete TLS 1.3 handshake against a real server and sends one HTTP
// request over it. Everything before this slice was checked against published
// vectors; this is the first time the certificate branch and the record
// reassembly run against a peer that was not written here.

#define PROBE_SERVER 0x0A000204u
#define PROBE_PORT 443u
#define PROBE_HOST "mich.test"
#define PROBE_HOST_LENGTH 9u

#define READY_ATTEMPTS 40u
#define READY_DELAY_TICKS 25u
#define IO_SPINS 40000u

static struct tls_client client;
static u8 outgoing[20480];
static u8 incoming[4096];

static void sleep_ticks(unsigned int ticks) {
    int timer = mich_timer_create();
    if (timer <= 0) return;
    if (!mich_timer_arm((unsigned int)timer, ticks, 0))
        mich_timer_wait((unsigned int)timer);
    mich_handle_close((unsigned int)timer);
}

static int wait_connected(unsigned int handle) {
    for (unsigned int spin = 0; spin < IO_SPINS; spin++) {
        struct mich_socket_stream_state_result state;
        state.readiness = 0;
        if (mich_socket_stream_state(handle, &state)) return -1;
        if (state.readiness & (SOCKET_READY_ERROR | SOCKET_READY_HANGUP))
            return -1;
        if (state.readiness & SOCKET_READY_CONNECTED) return 0;
        mich_yield();
    }
    return -1;
}

static int send_all(unsigned int handle, const u8 *data, unsigned int length) {
    unsigned int sent = 0;
    while (sent < length) {
        struct mich_socket_stream_data chunk;
        unsigned int take = length - sent;
        if (take > MICH_SOCKET_STREAM_PAYLOAD_MAX)
            take = MICH_SOCKET_STREAM_PAYLOAD_MAX;
        chunk.length = (unsigned short)take;
        chunk.reserved = 0;
        for (unsigned int index = 0; index < take; index++)
            chunk.data[index] = data[sent + index];
        if (mich_socket_stream_send(handle, &chunk)) return -1;
        sent += take;
    }
    return 0;
}

// Reads whatever has arrived and feeds it to the state machine, which is the
// only place that knows where records and messages end.
static int pump(unsigned int handle, unsigned int spins) {
    for (unsigned int spin = 0; spin < spins; spin++) {
        struct mich_socket_stream_state_result state;
        state.readiness = 0;
        state.eof = 0;
        if (mich_socket_stream_state(handle, &state)) return -1;
        if (state.readiness & SOCKET_READY_ERROR) return -1;
        if (state.readiness & SOCKET_READY_READABLE) {
            struct mich_socket_stream_data chunk;
            chunk.length = 0;
            chunk.reserved = 0;
            if (!mich_socket_stream_receive(handle, &chunk) && chunk.length) {
                // Let the driver capsule run before the next read: a record
                // that is still in flight otherwise never arrives, and the
                // reassembly stalls waiting for bytes nobody is delivering.
                mich_yield();
                if (tls_client_feed(&client, chunk.data, chunk.length))
                    return -1;
                if (client.state == TLS_STATE_CONNECTED) return 0;
            }
        }
        if (state.eof) return -1;
        // The frames are delivered by the driver capsule, so every turn has to
        // yield. Spinning here starves the task that would hand over the rest
        // of the handshake.
        mich_yield();
    }
    return -1;
}

int main(void) {
    static struct x509_trust_store store;
    store.anchors = test_anchors;
    store.count = TEST_ANCHOR_COUNT;

    // The wall clock read from the CMOS chip back in the RTC slice is what
    // decides whether a certificate is inside its validity window.
    unsigned long long now = mich_wall_clock();
    if (!now) {
        mich_write("Mich tlsprobe: wall clock FAIL\n");
        return 1;
    }

    u8 private_key[TLS_SHARE_SIZE];
    u8 random[TLS_RANDOM_SIZE];
    u8 session_id[TLS_SESSION_ID_SIZE];
    if (getrandom(private_key, sizeof(private_key), 0) !=
            (long)sizeof(private_key) ||
        getrandom(random, sizeof(random), 0) != (long)sizeof(random) ||
        getrandom(session_id, sizeof(session_id), 0) !=
            (long)sizeof(session_id)) {
        mich_write("Mich tlsprobe: entropy FAIL\n");
        return 1;
    }
    mich_write("Mich tlsprobe: entropy ready\n");

    int handle = -1;
    for (unsigned int attempt = 0; attempt < READY_ATTEMPTS; attempt++) {
        handle = mich_socket_stream_create();
        if (handle <= 0) return 1;
        struct mich_socket_stream_connect_request connect;
        connect.interface_handle = 0;
        connect.destination_address = PROBE_SERVER;
        connect.destination_port = PROBE_PORT;
        connect.reserved = 0;
        if (!mich_socket_stream_connect((unsigned int)handle, &connect) &&
            !wait_connected((unsigned int)handle))
            break;
        mich_handle_close((unsigned int)handle);
        handle = -1;
        sleep_ticks(READY_DELAY_TICKS);
    }
    if (handle <= 0) {
        mich_write("Mich tlsprobe: connect FAIL\n");
        return 1;
    }
    mich_write("Mich tlsprobe: tcp connected\n");

    if (tls_client_init(&client, PROBE_HOST, PROBE_HOST_LENGTH, private_key,
                        random, session_id, TLS_SESSION_ID_SIZE, &store,
                        now)) {
        mich_write("Mich tlsprobe: init FAIL\n");
        return 1;
    }
    crypto_zero(private_key, sizeof(private_key));

    unsigned int hello_length = 4096;
    static u8 hello[4096];
    if (tls_client_write_hello(&client, hello, &hello_length)) {
        mich_write("Mich tlsprobe: ClientHello FAIL\n");
        return 1;
    }
    // The first flight travels in the clear, wrapped in a plain record.
    outgoing[0] = TLS_CONTENT_HANDSHAKE;
    outgoing[1] = 0x03;
    outgoing[2] = 0x01;
    outgoing[3] = (u8)(hello_length >> 8);
    outgoing[4] = (u8)hello_length;
    for (unsigned int index = 0; index < hello_length; index++)
        outgoing[TLS_RECORD_HEADER_SIZE + index] = hello[index];
    if (send_all((unsigned int)handle, outgoing,
                 TLS_RECORD_HEADER_SIZE + hello_length)) {
        mich_write("Mich tlsprobe: send ClientHello FAIL\n");
        return 1;
    }
    mich_write("Mich tlsprobe: ClientHello sent\n");

    if (pump((unsigned int)handle, IO_SPINS)) {
        mich_write("Mich tlsprobe: handshake FAIL\n");
        return 1;
    }
    mich_write("Mich tlsprobe: server certificate accepted\n");
    mich_write("Mich tlsprobe: server Finished verified\n");

    unsigned int finished_length = 4096;
    if (tls_client_write_finished(&client, hello, &finished_length)) {
        mich_write("Mich tlsprobe: client Finished FAIL\n");
        return 1;
    }
    unsigned int record_length = sizeof(outgoing);
    if (tls_client_seal(&client, TLS_CONTENT_HANDSHAKE, hello,
                        finished_length, outgoing, &record_length) ||
        send_all((unsigned int)handle, outgoing, record_length)) {
        mich_write("Mich tlsprobe: send Finished FAIL\n");
        return 1;
    }
    if (tls_client_activate_application_keys(&client)) {
        mich_write("Mich tlsprobe: key switch FAIL\n");
        return 1;
    }
    mich_write("Mich tlsprobe: handshake complete\n");

    static const char request[] =
        "GET / HTTP/1.1\r\nHost: " PROBE_HOST "\r\nConnection: close\r\n\r\n";
    record_length = sizeof(outgoing);
    if (tls_client_seal(&client, TLS_CONTENT_APPLICATION_DATA,
                        (const u8 *)request, sizeof(request) - 1u, outgoing,
                        &record_length) ||
        send_all((unsigned int)handle, outgoing, record_length)) {
        mich_write("Mich tlsprobe: request FAIL\n");
        return 1;
    }
    mich_write("Mich tlsprobe: request sent\n");

    // The reply arrives as records like any other traffic, so the same
    // reassembly is reused and the decrypted payload is inspected here.
    unsigned int buffered = 0;
    for (unsigned int spin = 0; spin < IO_SPINS; spin++) {
        struct mich_socket_stream_state_result state;
        state.readiness = 0;
        state.eof = 0;
        if (mich_socket_stream_state((unsigned int)handle, &state)) break;
        if (state.readiness & SOCKET_READY_READABLE) {
            struct mich_socket_stream_data chunk;
            chunk.length = 0;
            chunk.reserved = 0;
            if (!mich_socket_stream_receive((unsigned int)handle, &chunk) &&
                chunk.length) {
                for (unsigned int index = 0;
                     index < chunk.length && buffered < sizeof(incoming);
                     index++)
                    incoming[buffered++] = chunk.data[index];
                // Same reason as during the handshake: the capsule needs a
                // turn or the rest of the reply never shows up.
                mich_yield();
            }
        }
        if (buffered >= TLS_RECORD_HEADER_SIZE) {
            unsigned int body = ((unsigned int)incoming[3] << 8) | incoming[4];
            if (buffered >= TLS_RECORD_HEADER_SIZE + body) {
                static u8 payload[4096];
                unsigned int payload_length = 0;
                u8 type = 0;
                if (!tls_client_open(&client, incoming,
                                     TLS_RECORD_HEADER_SIZE + body, payload,
                                     &payload_length, &type) &&
                    type == TLS_CONTENT_APPLICATION_DATA &&
                    payload_length >= 12u &&
                    // The server answers with its own minor version, so only
                    // the family and the status code are checked.
                    crypto_equal(payload, "HTTP/1.", 7u) &&
                    crypto_equal(payload + 9u, "200", 3u)) {
                    mich_write("Mich tlsprobe: HTTP response received\n");
                    mich_write("Mich tlsprobe: https pass\n");
                    return 0;
                }
                // Anything else is either a ticket or a record this probe does
                // not care about; drop it and keep reading.
                unsigned int rest = buffered - TLS_RECORD_HEADER_SIZE - body;
                for (unsigned int index = 0; index < rest; index++)
                    incoming[index] = incoming[TLS_RECORD_HEADER_SIZE + body +
                                               index];
                buffered = rest;
            }
        }
        if (state.eof) break;
        mich_yield();
    }
    mich_write("Mich tlsprobe: response FAIL\n");
    return 1;
}
