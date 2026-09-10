#include "capsule.h"

#include <mich/syscall.h>
#include <mich/net_interface.h>
#include <mich/socket.h>

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
        listen.local_port = 8082;
        listen.backlog = 8;
        if (!mich_socket_stream_listen(
                (unsigned int)listener, &listen)) {
            capsule->listener_handle = (unsigned int)listener;
            mich_write("Mich virtio-net: passive listener ready\n");
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
    if (!mich_net_interface_ipv6_send_echo(
            capsule->interface_handle)) {
        capsule->ipv6_ping_sent = 1;
        mich_write("Mich virtio-net: external IPv6 ping queued\n");
    }
    if (!mich_net_interface_udpv6_start_probe(
            capsule->interface_handle)) {
        capsule->udpv6_probe_sent = 1;
        mich_write("Mich virtio-net: external UDPv6 queued\n");
    }
    if (!start_socket_udpv6(capsule, &ipv6))
        mich_write("Mich virtio-net: external IPv6 socket queued\n");
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
            struct mich_socket_stream_data data;
            data.length = 8;
            data.reserved = 0;
            for (unsigned int index = 0;
                 index < MICH_SOCKET_STREAM_PAYLOAD_MAX; index++)
                data.data[index] = 0;
            data.data[0] = 'S'; data.data[1] = 'T';
            data.data[2] = 'R'; data.data[3] = 'E';
            data.data[4] = 'A'; data.data[5] = 'M';
            data.data[6] = '5'; data.data[7] = 0;
            if (!mich_socket_stream_send(capsule->stream_handle, &data)) {
                capsule->stream_sent = 1;
                mich_write("Mich virtio-net: stream socket send pass\n");
            }
        }
    }
    if (capsule->stream_sent && !capsule->stream_complete) {
        struct mich_socket_stream_state_result state;
        state.state = 0;
        state.readiness = 0;
        state.error = 0;
        state.eof = 0;
        struct mich_socket_stream_data data;
        data.length = 0;
        data.reserved = 0;
        if (!mich_socket_stream_state(capsule->stream_handle, &state) &&
            (state.readiness & SOCKET_READY_READABLE) &&
            !mich_socket_stream_receive(capsule->stream_handle, &data) &&
            data.length == 8 && data.data[0] == 'S' &&
            data.data[1] == 'T' && data.data[2] == 'R' &&
            data.data[3] == 'E' && data.data[4] == 'A' &&
            data.data[5] == 'M' && data.data[6] == '5') {
            if (!capsule->stream_rounds)
                mich_write("Mich virtio-net: stream readiness readable pass\n");
            capsule->stream_rounds++;
            if (capsule->stream_rounds == 1)
                mich_write("Mich virtio-net: stream socket receive pass\n");
            if (capsule->stream_rounds < 32) {
                if (mich_socket_stream_send(capsule->stream_handle, &data))
                    return -1;
            } else {
                capsule->stream_complete = 1;
                mich_write("Mich virtio-net: TCP stream soak pass\n");
                if (!mich_socket_stream_shutdown(capsule->stream_handle)) {
                    capsule->stream_shutdown = 1;
                    mich_write("Mich virtio-net: stream socket shutdown pass\n");
                }
            }
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
    if (capsule->listener_handle && !capsule->accepted_handle) {
        int accepted = mich_socket_stream_accept(capsule->listener_handle);
        if (accepted > 0) {
            capsule->accepted_handle = (unsigned int)accepted;
            mich_write("Mich virtio-net: external passive accept pass\n");
        }
    }
    if (capsule->accepted_handle && !capsule->passive_complete) {
        struct mich_socket_stream_data data;
        data.length = 0;
        data.reserved = 0;
        if (!mich_socket_stream_receive(capsule->accepted_handle, &data) &&
            data.length == 7 && data.data[0] == 'P' &&
            data.data[1] == 'A' && data.data[2] == 'S' &&
            data.data[3] == 'S' && data.data[4] == 'I' &&
            data.data[5] == 'V' && data.data[6] == 'E' &&
            !mich_socket_stream_send(capsule->accepted_handle, &data)) {
            capsule->passive_complete = 1;
            mich_write("Mich virtio-net: external passive echo pass\n");
        }
    }
    if (capsule->ipv6_dad_complete && !capsule->ipv6_slaac_ready)
        probes_on_slaac(capsule);
    if (capsule->ipv6_ping_sent && !capsule->ipv6_ping_complete &&
        mich_net_interface_ipv6_echo_replies(
            capsule->interface_handle)) {
        capsule->ipv6_ping_complete = 1;
        mich_write("Mich virtio-net: external IPv6 ping reply pass\n");
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
        }
    }
    return 0;
}
