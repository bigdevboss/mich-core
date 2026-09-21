#include "capsule.h"

#include <mich/syscall.h>
#include <mich/net_interface.h>
#include <mich/socket.h>
#include <mich/timer.h>

static const unsigned char passive_message[] = {
    'P', 'A', 'S', 'S', 'I', 'V', 'E'
};
static const unsigned char stream_message[] = {
    'S', 'T', 'R', 'E', 'A', 'M', '5', 0
};

#define STREAM_ROUNDS 32

static void append_snapshot_text(char *text, unsigned int *length,
                                 const char *value) {
    for (unsigned int index = 0; value[index] && *length < 158; index++)
        text[(*length)++] = value[index];
}

static void append_snapshot_hex(char *text, unsigned int *length,
                                unsigned int value) {
    for (int shift = 28; shift >= 0 && *length < 158; shift -= 4) {
        unsigned int digit = (value >> shift) & 15u;
        text[(*length)++] = digit < 10 ? (char)('0' + digit) :
                                     (char)('A' + digit - 10);
    }
}

static void report_passive_snapshot(struct virtio_net_capsule *capsule,
                                    unsigned int handle, const char *phase,
                                    unsigned int rx_progress) {
    if (!capsule || !capsule->timing_enabled || !handle) return;
    struct mich_socket_stream_state_result state;
    state.state = 0;
    state.readiness = 0;
    state.error = 0;
    state.eof = 0;
    unsigned int queried = !mich_socket_stream_state(handle, &state);
    char text[160];
    unsigned int length = 0;
    append_snapshot_text(text, &length, "Mich virtio-net: passive snapshot ");
    append_snapshot_text(text, &length, phase);
    append_snapshot_text(text, &length, " query=0x");
    append_snapshot_hex(text, &length, queried);
    append_snapshot_text(text, &length, " state=0x");
    append_snapshot_hex(text, &length, state.state);
    append_snapshot_text(text, &length, " ready=0x");
    append_snapshot_hex(text, &length, state.readiness);
    append_snapshot_text(text, &length, " error=0x");
    append_snapshot_hex(text, &length, (unsigned int)state.error);
    append_snapshot_text(text, &length, " eof=0x");
    append_snapshot_hex(text, &length, state.eof);
    append_snapshot_text(text, &length, " rx-progress=0x");
    append_snapshot_hex(text, &length, rx_progress ? 1 : 0);
    text[length++] = '\n';
    text[length] = 0;
    mich_write(text);
}

static inline int queue_stream_message(struct virtio_net_capsule *capsule) {
    struct mich_socket_stream_data data;
    data.length = sizeof(stream_message);
    data.reserved = 0;
    for (unsigned int index = 0; index < data.length; index++)
        data.data[index] = stream_message[index];
    if (mich_socket_stream_send(capsule->stream_handle, &data)) return -1;
    capsule->stream_sent++;
    return 0;
}

static int start_socket_udp(struct virtio_net_capsule *capsule) {
    int handle = mich_socket_create();
    if (handle <= 0) return -1;
    struct mich_socket_bind_request bind;
    bind.address = capsule->offered_address;
    bind.port = 0;
    bind.reserved = 0;
    if (mich_socket_bind((unsigned int)handle, &bind)) return -1;
    static struct mich_socket_send_request request;
    request.destination_address = capsule->offered_dns;
    request.destination_port = 53;
    request.length = 29;
    for (unsigned int index = 0; index < MICH_SOCKET_PAYLOAD_MAX; index++)
        request.payload[index] = 0;
    request.payload[0] = 0x4D;
    request.payload[1] = 0x53;
    request.payload[2] = 0x01;
    request.payload[5] = 1;
    request.payload[12] = 7;
    request.payload[13] = 'e';
    request.payload[14] = 'x';
    request.payload[15] = 'a';
    request.payload[16] = 'm';
    request.payload[17] = 'p';
    request.payload[18] = 'l';
    request.payload[19] = 'e';
    request.payload[20] = 3;
    request.payload[21] = 'c';
    request.payload[22] = 'o';
    request.payload[23] = 'm';
    request.payload[25] = 0;
    request.payload[26] = 1;
    request.payload[27] = 0;
    request.payload[28] = 1;
    if (mich_socket_send_to((unsigned int)handle, &request)) return -1;
    capsule->socket_handle = (unsigned int)handle;
    capsule->socket_udp_sent = 1;
    return 0;
}

static int poll_socket_udp(struct virtio_net_capsule *capsule) {
    static struct mich_socket_receive_result result;
    if (!capsule->socket_handle ||
        mich_socket_receive_from(capsule->socket_handle, &result))
        return 0;
    return result.source_port == 53 && result.length >= 12 &&
        result.payload[0] == 0x4D && result.payload[1] == 0x53 &&
        (result.payload[2] & 0x80) ? 1 : -1;
}

static int start_socket_udpv6(
    struct virtio_net_capsule *capsule,
    const struct mich_net_interface_ipv6_info *info) {
    int handle = mich_socket_ipv6_create();
    if (handle <= 0) return -1;
    struct mich_socket_ipv6_bind_request bind;
    for (unsigned int index = 0; index < 16; index++)
        bind.address[index] = info->global[index];
    bind.port = 0;
    bind.reserved = 0;
    if (mich_socket_ipv6_bind((unsigned int)handle, &bind)) return -1;
    static struct mich_socket_ipv6_send_request request;
    for (unsigned int index = 0; index < 8; index++)
        request.destination_address[index] = info->global[index];
    for (unsigned int index = 8; index < 16; index++)
        request.destination_address[index] = 0;
    request.destination_address[15] = 3;
    request.destination_port = 53;
    request.length = 29;
    for (unsigned int index = 0; index < MICH_SOCKET_PAYLOAD_MAX; index++)
        request.payload[index] = 0;
    request.payload[0] = 0x4D;
    request.payload[1] = 0x53;
    request.payload[2] = 0x01;
    request.payload[5] = 1;
    request.payload[12] = 7;
    request.payload[13] = 'e';
    request.payload[14] = 'x';
    request.payload[15] = 'a';
    request.payload[16] = 'm';
    request.payload[17] = 'p';
    request.payload[18] = 'l';
    request.payload[19] = 'e';
    request.payload[20] = 3;
    request.payload[21] = 'c';
    request.payload[22] = 'o';
    request.payload[23] = 'm';
    request.payload[25] = 0;
    request.payload[26] = 1;
    request.payload[27] = 0;
    request.payload[28] = 1;
    if (mich_socket_ipv6_send_to((unsigned int)handle, &request)) return -1;
    capsule->socket6_handle = (unsigned int)handle;
    capsule->socket6_sent = 1;
    return 0;
}

void probes_on_ipv4_up(struct virtio_net_capsule *capsule) {
    if (capsule->external_probe_enabled) {
        struct mich_net_interface_echo_request echo;
        echo.destination = capsule->offered_gateway;
        echo.identifier = 0x4D49;
        echo.sequence = 1;
        if (mich_net_interface_send_echo(
                capsule->interface_handle, &echo) >= 0) {
            capsule->ping_sent = 1;
            mich_write("Mich virtio-net: external ping queued\n");
        }
        if (capsule->offered_dns &&
            !mich_net_interface_send_udp_probe(
                capsule->interface_handle,
                capsule->offered_dns)) {
            capsule->udp_probe_sent = 1;
            mich_write("Mich virtio-net: external UDP queued\n");
        }
        if (capsule->offered_dns &&
            !start_socket_udp(capsule))
            mich_write("Mich virtio-net: external socket UDP queued\n");
        if (!mich_net_interface_tcp_probe_start(
                capsule->interface_handle,
                0x0A000204u, 8080)) {
            capsule->tcp_probe_started = 1;
            mich_write("Mich virtio-net: external TCP SYN queued\n");
        }
        int stream = mich_socket_stream_create();
        if (stream > 0) {
            struct mich_socket_stream_connect_request connect;
            connect.interface_handle = capsule->interface_handle;
            connect.destination_address = 0x0A000204u;
            connect.destination_port = 8080;
            connect.reserved = 0;
            if (!mich_socket_stream_connect(
                    (unsigned int)stream, &connect)) {
                capsule->stream_handle = (unsigned int)stream;
                mich_write("Mich virtio-net: stream socket connect queued\n");
            }
        }
    }
    int listener = mich_socket_stream_create();
    if (listener > 0) {
        struct mich_socket_stream_listen_request listen;
        listen.interface_handle = capsule->interface_handle;
        listen.local_port = VIRTIO_NET_PASSIVE_PORT;
        listen.backlog = 8;
        if (!mich_socket_stream_listen(
                (unsigned int)listener, &listen)) {
            capsule->listener_handle = (unsigned int)listener;
            if (capsule->timing_enabled) {
                capsule->listener_timer_snapshot_pending = 1;
                capsule->listener_rx_packets = capsule->rx_packets;
                capsule->listener_rx_drops = capsule->rx_drops;
            }
            mich_write("Mich virtio-net: passive listener ready\n");
            virtio_net_timing_mark(capsule, "passive-listener");
        }
    }
}

void probes_on_slaac(struct virtio_net_capsule *capsule) {
    struct mich_net_interface_ipv6_info ipv6;
    if (mich_net_interface_ipv6_get_info(
            capsule->interface_handle, &ipv6) ||
        ipv6.state != NET_INTERFACE_ABI_IPV6_SLAAC)
        return;
    capsule->ipv6_slaac_ready = 1;
    mich_write("Mich virtio-net: external IPv6 RA and SLAAC pass\n");
    virtio_net_timing_mark(capsule, "ipv6-echo-initial");
    if (!mich_net_interface_ipv6_send_echo(
            capsule->interface_handle)) {
        capsule->ipv6_ping_sent = 1;
        capsule->ipv6_ping_retries = 0;
        capsule->ipv6_ping_retry_at = mich_ticks() + 1000;
        mich_write("Mich virtio-net: external IPv6 ping queued\n");
    }
    if (!mich_net_interface_udpv6_start_probe(
            capsule->interface_handle)) {
        capsule->udpv6_probe_sent = 1;
        capsule->udpv6_probe_retries = 0;
        capsule->udpv6_probe_retry_at = mich_ticks() + 1000;
        mich_write("Mich virtio-net: external UDPv6 queued\n");
    }
    if (!start_socket_udpv6(capsule, &ipv6))
        mich_write("Mich virtio-net: external IPv6 socket queued\n");
}

int probes_external_complete(const struct virtio_net_capsule *capsule) {
    if (!capsule) return 0;
    return capsule->external_probe_enabled & capsule->ipv4_configured &
        capsule->ping_complete & capsule->udp_probe_complete &
        capsule->socket_udp_complete & capsule->tcp_probe_complete &
        capsule->stream_closed &
        (capsule->passive_received == sizeof(passive_message) + 1) &
        capsule->passive_closed & capsule->ipv6_dad_complete &
        capsule->ipv6_slaac_ready &
        capsule->ipv6_ping_complete & capsule->udpv6_probe_complete &
        capsule->socket6_sent & capsule->interrupt_seen &
        capsule->batch_reported & capsule->tx_batch_reported &
        capsule->itr_reported;
}

int probes_poll(struct virtio_net_capsule *capsule) {
    if (capsule->ping_sent && !capsule->ping_complete &&
        mich_net_interface_echo_replies(capsule->interface_handle)) {
        capsule->ping_complete = 1;
        mich_write("Mich virtio-net: external ping reply pass\n");
    }
    if (capsule->udp_probe_sent && !capsule->udp_probe_complete &&
        mich_net_interface_poll_udp_probe(
            capsule->interface_handle) == 1) {
        capsule->udp_probe_complete = 1;
        mich_write("Mich virtio-net: external UDP reply pass\n");
    }
    if (capsule->socket_udp_sent && !capsule->socket_udp_complete &&
        poll_socket_udp(capsule) == 1) {
        capsule->socket_udp_complete = 1;
        mich_write("Mich virtio-net: external socket UDP reply pass\n");
    }
    if (capsule->tcp_probe_started && !capsule->tcp_probe_complete &&
        mich_net_interface_tcp_probe_poll(
            capsule->interface_handle) == 1) {
        capsule->tcp_probe_complete = 1;
        mich_write("Mich virtio-net: external TCP handshake and echo pass\n");
    }
    if (capsule->stream_handle && !capsule->stream_sent) {
        struct mich_socket_stream_state_result state;
        state.state = 0;
        state.readiness = 0;
        state.error = 0;
        state.eof = 0;
        if (!mich_socket_stream_state(capsule->stream_handle, &state) &&
            state.state == 4 &&
            (state.readiness & SOCKET_READY_CONNECTED) &&
            (state.readiness & SOCKET_READY_WRITABLE)) {
            if (!capsule->readiness_reported) {
                capsule->readiness_reported = 1;
                mich_write("Mich virtio-net: stream readiness connected pass\n");
            }
            for (unsigned int index = 0; index < STREAM_ROUNDS; index++)
                if (queue_stream_message(capsule)) return -1;
            mich_write("Mich virtio-net: stream socket send pass\n");
        }
    }
    if (capsule->stream_sent && !capsule->stream_complete) {
        for (unsigned int read = 0; read < STREAM_ROUNDS; read++) {
            struct mich_socket_stream_state_result state;
            state.state = 0;
            state.readiness = 0;
            state.error = 0;
            state.eof = 0;
            struct mich_socket_stream_data data;
            data.length = 0;
            data.reserved = 0;
            if (mich_socket_stream_state(capsule->stream_handle, &state) ||
                !(state.readiness & SOCKET_READY_READABLE) ||
                mich_socket_stream_receive(capsule->stream_handle, &data))
                break;
            for (unsigned int index = 0; index < data.length; index++) {
                if (data.data[index] !=
                    stream_message[capsule->stream_received])
                    return -1;
                capsule->stream_received++;
                if (capsule->stream_received != sizeof(stream_message))
                    continue;
                capsule->stream_received = 0;
                if (!capsule->stream_rounds)
                    mich_write("Mich virtio-net: stream readiness readable pass\n");
                capsule->stream_rounds++;
                if (capsule->stream_rounds == 1)
                    mich_write("Mich virtio-net: stream socket receive pass\n");
                if (capsule->stream_sent < STREAM_ROUNDS &&
                    queue_stream_message(capsule))
                    return -1;
                if (capsule->stream_rounds == STREAM_ROUNDS) {
                    capsule->stream_complete = 1;
                    mich_write("Mich virtio-net: TCP stream soak pass\n");
                    if (!mich_socket_stream_shutdown(capsule->stream_handle)) {
                        capsule->stream_shutdown = 1;
                        mich_write("Mich virtio-net: stream socket shutdown pass\n");
                    }
                }
            }
            if (capsule->stream_complete) break;
        }
    }
    if (capsule->stream_shutdown && !capsule->stream_closed) {
        struct mich_socket_stream_state_result state;
        state.state = 0;
        state.readiness = 0;
        state.error = 0;
        state.eof = 0;
        if (!mich_socket_stream_state(capsule->stream_handle, &state) &&
            (state.readiness & SOCKET_READY_HANGUP)) {
            capsule->stream_closed = 1;
            mich_write("Mich virtio-net: external TCP FIN lifecycle pass\n");
        }
    }
    if (capsule->timing_enabled && capsule->listener_snapshot_due &&
        !capsule->accepted_handle && !capsule->listener_snapshot_reported) {
        capsule->listener_snapshot_due = 0;
        capsule->listener_snapshot_reported = 1;
        report_passive_snapshot(
            capsule, capsule->listener_handle, "listener",
            capsule->rx_packets != capsule->listener_rx_packets ||
            capsule->rx_drops != capsule->listener_rx_drops);
    }
    if (capsule->listener_handle && !capsule->accepted_handle) {
        int accepted = mich_socket_stream_accept(capsule->listener_handle);
        if (accepted > 0) {
            capsule->accepted_handle = (unsigned int)accepted;
            mich_write("Mich virtio-net: external passive accept pass\n");
            virtio_net_timing_mark(capsule, "passive-accept");
        }
    }
    if (capsule->accepted_handle && capsule->passive_received < 8) {
        struct mich_socket_stream_data data;
        data.length = 0;
        data.reserved = 0;
        if (capsule->timing_enabled && !capsule->passive_receive_polled) {
            capsule->passive_receive_polled = 1;
            virtio_net_timing_mark(capsule, "passive-receive");
        }
        int receive_result = -1;
        if (capsule->passive_received < sizeof(passive_message))
            receive_result = mich_socket_stream_receive(
                capsule->accepted_handle, &data);
        if (capsule->timing_enabled && !receive_result && !data.length &&
            !capsule->passive_empty_snapshot_reported) {
            capsule->passive_empty_snapshot_reported = 1;
            capsule->passive_rx_packets = capsule->rx_packets;
            capsule->passive_rx_drops = capsule->rx_drops;
            report_passive_snapshot(
                capsule, capsule->accepted_handle, "empty", 0);
        }
        if (capsule->timing_enabled && capsule->passive_wake_reported &&
            capsule->passive_empty_snapshot_reported &&
            !capsule->passive_wake_snapshot_reported) {
            capsule->passive_wake_snapshot_reported = 1;
            report_passive_snapshot(
                capsule, capsule->accepted_handle, "wake",
                capsule->rx_packets != capsule->passive_rx_packets ||
                capsule->rx_drops != capsule->passive_rx_drops);
        }
        if (!receive_result) {
            if (data.length)
                virtio_net_timing_mark(capsule, "passive-bytes");
            if (data.length > sizeof(passive_message) -
                              capsule->passive_received) {
                capsule->passive_received = 9;
            } else {
                for (unsigned int index = 0; index < data.length; index++)
                    if (data.data[index] !=
                        passive_message[capsule->passive_received + index])
                        capsule->passive_received = 9;
                if (capsule->passive_received < sizeof(passive_message))
                    capsule->passive_received += data.length;
            }
        }
        if (capsule->passive_received == sizeof(passive_message)) {
            data.length = sizeof(passive_message);
            for (unsigned int index = 0; index < data.length; index++)
                data.data[index] = passive_message[index];
            if (!mich_socket_stream_send(capsule->accepted_handle, &data)) {
                capsule->passive_received = 8;
                mich_write("Mich virtio-net: external passive echo pass\n");
                virtio_net_timing_mark(capsule, "passive-echo");
            }
        }
    }
    if (capsule->timing_enabled && capsule->accepted_handle &&
        capsule->passive_received < 8 && !capsule->passive_wait_reported) {
        capsule->passive_wait_reported = 1;
        virtio_net_timing_mark(capsule, "passive-wait");
    }
    if (capsule->accepted_handle &&
        capsule->passive_received == sizeof(passive_message) + 1 &&
        !capsule->passive_closed) {
        struct mich_socket_stream_state_result state;
        state.state = 0;
        state.readiness = 0;
        state.error = 0;
        state.eof = 0;
        if (!mich_socket_stream_state(capsule->accepted_handle, &state) &&
            (state.readiness & SOCKET_READY_HANGUP)) {
            capsule->passive_closed = 1;
            mich_write("Mich virtio-net: external passive close pass\n");
            virtio_net_timing_mark(capsule, "passive-close");
        }
    }
    if (capsule->ipv6_dad_complete && !capsule->ipv6_slaac_ready)
        probes_on_slaac(capsule);
    if (capsule->ipv6_ping_sent && !capsule->ipv6_ping_complete) {
        if (mich_net_interface_ipv6_echo_replies(
                capsule->interface_handle)) {
            capsule->ipv6_ping_complete = 1;
            mich_write("Mich virtio-net: external IPv6 ping reply pass\n");
            virtio_net_timing_mark(capsule, "ipv6-echo-reply");
        } else if (capsule->ipv6_ping_retries < 3) {
            unsigned int now = mich_ticks();
            if ((int)(now - capsule->ipv6_ping_retry_at) >= 0) {
                capsule->ipv6_ping_retry_at = now + 1000;
                capsule->ipv6_ping_retries++;
                if (capsule->ipv6_ping_retries == 1)
                    virtio_net_timing_mark(capsule, "ipv6-echo-retry-1");
                else if (capsule->ipv6_ping_retries == 2)
                    virtio_net_timing_mark(capsule, "ipv6-echo-retry-2");
                else
                    virtio_net_timing_mark(capsule, "ipv6-echo-retry-3");
                mich_net_interface_ipv6_send_echo(capsule->interface_handle);
            }
        }
    }
    if (capsule->udpv6_probe_sent && !capsule->udpv6_probe_complete) {
        int udpv6 = mich_net_interface_udpv6_poll_probe(
            capsule->interface_handle);
        if (udpv6 == 1) {
            capsule->udpv6_probe_complete = 1;
            mich_write("Mich virtio-net: external UDPv6 reply pass\n");
        } else if (udpv6 == 2) {
            capsule->udpv6_probe_complete = 1;
            mich_write("Mich virtio-net: external UDPv6 ICMP error pass\n");
        } else if (!udpv6 && capsule->udpv6_probe_retries < 3) {
            unsigned int now = mich_ticks();
            if ((int)(now - capsule->udpv6_probe_retry_at) >= 0) {
                capsule->udpv6_probe_retry_at = now + 1000;
                capsule->udpv6_probe_retries++;
                mich_net_interface_udpv6_start_probe(capsule->interface_handle);
            }
        }
    }
    return 0;
}
