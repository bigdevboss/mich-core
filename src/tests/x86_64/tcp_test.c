#include "net_test.h"

struct udpv6_test_link {
    struct ipv6_context *ipv6;
    u32 transmitted;
};

static int udpv6_test_transmit(
    const u8 source[16], const u8 destination[16],
    const void *datagram, u32 length, void *context) {
    struct udpv6_test_link *link = context;
    if (!link || !link->ipv6 || !source || !destination ||
        !datagram || length > UDPV6_HEADER_SIZE + UDPV6_PAYLOAD_MAX)
        return -1;
    u8 packet[IPV6_HEADER_SIZE + UDPV6_HEADER_SIZE + UDPV6_PAYLOAD_MAX];
    if (ipv6_build_header(packet, sizeof(packet), length,
                          source, destination, 17, 64, 0, 0))
        return -1;
    for (u32 index = 0; index < length; index++)
        packet[IPV6_HEADER_SIZE + index] = ((const u8 *)datagram)[index];
    link->transmitted++;
    return ipv6_receive(link->ipv6, packet, IPV6_HEADER_SIZE + length);
}

int test_udpv6(void) {
    const u8 mac[6] = {0x02, 0x4D, 0x49, 0x43, 0x48, 0x68};
    u8 address[16];
    static struct ethernet_port port;
    static struct ipv6_context ipv6;
    static struct udpv6_context udp;
    struct udpv6_test_link link;
    int valid = !ipv6_link_local_from_mac(mac, address) &&
        !ethernet_port_init(&port, mac) &&
        !ipv6_init(&ipv6, &port, address, 1);
    link.ipv6 = &ipv6;
    link.transmitted = 0;
    valid = valid && !udpv6_init(&udp, &ipv6,
                                 udpv6_test_transmit, &link);
    u64 sender = udpv6_bind(&udp, address, 14000);
    u64 receiver = udpv6_bind(&udp, address, 14001);
    u64 ephemeral = udpv6_bind(&udp, address, 0);
    u8 payload[32];
    for (u32 index = 0; index < sizeof(payload); index++) payload[index] = (u8)index;
    valid = valid && sender && receiver && ephemeral &&
        !udpv6_send(&udp, sender, address, 14001,
                    payload, sizeof(payload)) && link.transmitted == 1;
    struct udpv6_datagram datagram;
    valid = valid && !udpv6_receive(&udp, receiver, &datagram) &&
        datagram.source_port == 14000 && datagram.destination_port == 14001 &&
        datagram.length == sizeof(payload);
    for (u32 index = 0; index < sizeof(payload); index++)
        if (datagram.payload[index] != payload[index]) valid = 0;
    u8 raw[UDPV6_HEADER_SIZE + 1];
    for (u32 index = 0; index < sizeof(raw); index++) raw[index] = 0;
    raw[1] = 1;
    raw[3] = 2;
    raw[5] = sizeof(raw);
    valid = valid && udpv6_checksum(address, address, raw, sizeof(raw)) != 0 &&
        !udpv6_unbind(&udp, sender) && !udpv6_unbind(&udp, receiver) &&
        !udpv6_unbind(&udp, ephemeral) && udp.stats.transmitted == 1 &&
        udp.stats.received == 1 && udp.stats.bytes_transmitted == 32 &&
        udp.stats.bytes_received == 32;
    return valid ? 0 : -1;
}

static u32 hole_mtu;
static u32 hole_family;

static void hole_blackhole(u32 family, u32 address4, const u8 address6[16],
                           u32 mtu, void *context) {
    (void)address4;
    (void)address6;
    (void)context;
    hole_family = family;
    hole_mtu = mtu;
}

static u32 retransmission_deadline(const struct tcp_connection *c) {
    u32 deadline = 0;
    if (!c) return 0;
    for (u32 index = 0; index < TCP_RETRANSMISSION_MAX; index++)
        if (c->retransmissions[index].active)
            deadline = c->retransmissions[index].deadline;
    return deadline;
}

int test_tcp(void) {
    static struct tcp_context client;
    static struct tcp_context server;
    const u32 client_address = 0x0A000002u;
    const u32 server_address = 0x0A000003u;
    tcp_init(&client, 1000);
    tcp_init(&server, 9000);
    u64 listener = tcp_listen(&server, server_address, 8080);
    u64 active = tcp_active_open(&client, client_address, 50000,
                                 server_address, 8080);
    int valid = listener && active;
    u32 client_isn = active
        ? client.connections[(u32)active - 1].send_unacknowledged : 0;
    u8 segment[128];
    int length = tcp_build_ipv4(
        segment, sizeof(segment), client_address, server_address,
        50000, 8080, client_isn, 0, TCP_FLAG_SYN, 65535, 0, 0);
    struct tcp_response synack;
    valid = valid && length == TCP_HEADER_MIN &&
        !tcp_receive_ipv4(&server, client_address, server_address,
                          segment, (u32)length, &synack) && synack.valid &&
        synack.flags == (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
        synack.acknowledgement == client_isn + 1;
    length = tcp_build_ipv4(
        segment, sizeof(segment), server_address, client_address,
        synack.source_port, synack.destination_port,
        synack.sequence, synack.acknowledgement,
        synack.flags, synack.window, 0, 0);
    struct tcp_response ack;
    valid = valid && length == TCP_HEADER_MIN &&
        !tcp_receive_ipv4(&client, server_address, client_address,
                          segment, (u32)length, &ack) && ack.valid &&
        ack.flags == TCP_FLAG_ACK;
    length = tcp_build_ipv4(
        segment, sizeof(segment), client_address, server_address,
        ack.source_port, ack.destination_port,
        ack.sequence, ack.acknowledgement,
        ack.flags, ack.window, 0, 0);
    struct tcp_response none;
    valid = valid && !tcp_receive_ipv4(
        &server, client_address, server_address,
        segment, (u32)length, &none) && !none.valid;
    u32 client_state;
    valid = valid && !tcp_connection_state(&client, active, &client_state) &&
        client_state == TCP_STATE_ESTABLISHED;
    length = tcp_build_ipv4(
        segment, sizeof(segment), server_address, client_address,
        8080, 50000, 0xDEADBEEFu, client_isn + 1,
        TCP_FLAG_RST | TCP_FLAG_ACK, 0, 0, 0);
    struct tcp_response challenge;
    valid = valid && tcp_receive_ipv4(
        &client, server_address, client_address,
        segment, (u32)length, &challenge) < 0 && challenge.valid &&
        !tcp_connection_state(&client, active, &client_state) &&
        client_state == TCP_STATE_ESTABLISHED;
    struct tcp_connection *accepted = 0;
    u64 accepted_id = 0;
    for (u32 index = 0; index < TCP_CONNECTION_MAX; index++)
        if (server.connections[index].active &&
            server.connections[index].state == TCP_STATE_ESTABLISHED) {
            accepted = &server.connections[index];
            accepted_id = ((u64)accepted->generation << 32) | (index + 1);
        }
    valid = valid && accepted && accepted_id;
    u64 accepted_from_api = 0;
    int accept_result = tcp_accept(&server, listener, &accepted_from_api);
    int accept_again = tcp_accept(&server, listener, &accepted_from_api);
    valid = valid && !accept_result && accepted_from_api == accepted_id &&
        accept_again == 1;
    u8 payload[32];
    for (u32 index = 0; index < sizeof(payload); index++) payload[index] = (u8)index;
    length = tcp_build_ipv4(
        segment, sizeof(segment), client_address, server_address,
        50000, 8080, ack.sequence, ack.acknowledgement,
        TCP_FLAG_ACK | TCP_FLAG_PSH, 65535,
        payload, sizeof(payload));
    struct tcp_response data_ack;
    int data_result = tcp_receive_ipv4(
        &server, client_address, server_address,
        segment, (u32)length, &data_ack);
    valid = valid && length == TCP_HEADER_MIN + (int)sizeof(payload) &&
        !data_result && data_ack.valid && data_ack.flags == TCP_FLAG_ACK &&
        data_ack.acknowledgement == ack.sequence + sizeof(payload);
    u8 received[64];
    u32 received_length = 0;
    valid = valid && !tcp_receive_data(
        &server, accepted_id, received, sizeof(received), &received_length) &&
        received_length == sizeof(payload);
    for (u32 index = 0; index < sizeof(payload); index++)
        if (received[index] != payload[index]) valid = 0;
    valid = valid && !tcp_queue_send(&client, active, payload, sizeof(payload));
    struct tcp_transmit transmit;
    int prepare_result = tcp_prepare_transmit(&client, active, 10, &transmit);
    valid = valid && prepare_result == 1 &&
        transmit.length == sizeof(payload) && !transmit.retransmission;
    struct tcp_transmit retransmit;
    u32 retransmit_deadline = 0;
    for (u32 index = 0; index < TCP_RETRANSMISSION_MAX; index++)
        if (client.connections[(u32)active - 1].retransmissions[index].active)
            retransmit_deadline =
                client.connections[(u32)active - 1].retransmissions[index].deadline;
    valid = valid && retransmit_deadline &&
        tcp_tick(&client, retransmit_deadline - 1, &retransmit) == 0 &&
        tcp_tick(&client, retransmit_deadline, &retransmit) == 1 &&
        retransmit.retransmission && retransmit.sequence == transmit.sequence &&
        retransmit.length == transmit.length && client.stats.retransmissions == 1;
    length = tcp_build_ipv4(
        segment, sizeof(segment), server_address, client_address,
        8080, 50000, client.connections[(u32)active - 1].receive_next,
        transmit.sequence + transmit.length, TCP_FLAG_ACK, 65535, 0, 0);
    valid = valid && length == TCP_HEADER_MIN &&
        !tcp_receive_ipv4(&client, server_address, client_address,
                          segment, (u32)length, &none) &&
        client.connections[(u32)active - 1].send_unacknowledged ==
            transmit.sequence + transmit.length;
    u32 base = accepted->receive_next;
    u8 second[8];
    u8 first[8];
    for (u32 index = 0; index < 8; index++) {
        first[index] = (u8)(0xA0 + index);
        second[index] = (u8)(0xB0 + index);
    }
    length = tcp_build_ipv4(segment, sizeof(segment),
        client_address, server_address, 50000, 8080,
        base + 8, 0, TCP_FLAG_ACK | TCP_FLAG_PSH, 65535, second, 8);
    valid = valid && !tcp_receive_ipv4(&server, client_address, server_address,
                                       segment, (u32)length, &none) &&
        server.stats.out_of_order_queued == 1;
    length = tcp_build_ipv4(segment, sizeof(segment),
        client_address, server_address, 50000, 8080,
        base, 0, TCP_FLAG_ACK | TCP_FLAG_PSH, 65535, first, 8);
    valid = valid && !tcp_receive_ipv4(&server, client_address, server_address,
                                       segment, (u32)length, &none) &&
        accepted->receive_next == base + 16;
    received_length = 0;
    valid = valid && !tcp_receive_data(
        &server, accepted_id, received, sizeof(received), &received_length) &&
        received_length == 16;
    for (u32 index = 0; index < 8; index++)
        if (received[index] != first[index] || received[index + 8] != second[index])
            valid = 0;
    u8 options[40];
    for (u32 index = 0; index < sizeof(options); index++) options[index] = 0;
    options[0] = 0x30;
    options[1] = 0x39;
    options[2] = 0x1F;
    options[3] = 0x90;
    options[12] = 10 << 4;
    options[13] = TCP_FLAG_SYN;
    options[20] = 2;
    options[21] = 4;
    options[22] = 0x05;
    options[23] = 0xB4;
    options[24] = 1;
    options[25] = 3;
    options[26] = 3;
    options[27] = 7;
    options[28] = 4;
    options[29] = 2;
    options[30] = 8;
    options[31] = 10;
    options[35] = 1;
    options[39] = 2;
    struct tcp_segment_view view;
    valid = valid && !tcp_parse(options, sizeof(options), &view) &&
        view.header_length == 40 && view.mss == 1460 &&
        view.window_scale == 7 && view.sack_permitted &&
        view.timestamp_present && view.timestamp_value == 1 &&
        view.timestamp_echo == 2;
    options[12] = 4 << 4;
    valid = valid && tcp_parse(options, sizeof(options), &view) < 0 &&
        client.stats.connections_opened == 1 &&
        server.stats.connections_opened == 1;
    u8 source6[16];
    u8 destination6[16];
    for (u32 index = 0; index < 16; index++) {
        source6[index] = 0;
        destination6[index] = 0;
    }
    source6[0] = 0xFE; source6[1] = 0x80; source6[15] = 2;
    destination6[0] = 0xFE; destination6[1] = 0x80; destination6[15] = 3;
    length = tcp_build_ipv6(
        segment, sizeof(segment), source6, destination6,
        50000, 8080, 123, 456, TCP_FLAG_ACK,
        32768, payload, sizeof(payload));
    valid = valid && length == TCP_HEADER_MIN + (int)sizeof(payload) &&
        !tcp_checksum_ipv6(source6, destination6, segment, (u32)length) &&
        !tcp_parse(segment, (u32)length, &view) &&
        view.sequence == 123 && view.acknowledgement == 456;
    static struct tcp_context tcp6;
    tcp_init(&tcp6, 6000);
    u64 connection6 = tcp_active_open_ipv6(
        &tcp6, source6, 51000, destination6, 8081);
    struct tcp_transmit syn6;
    valid = valid && connection6 &&
        tcp_prepare_transmit(&tcp6, connection6, 0, &syn6) == 1 &&
        syn6.family == 6 && syn6.flags == TCP_FLAG_SYN;
    length = tcp_build_ipv6(
        segment, sizeof(segment), destination6, source6,
        8081, 51000, 7000, syn6.sequence + 1,
        TCP_FLAG_SYN | TCP_FLAG_ACK, 65535, 0, 0);
    struct tcp_response ack6;
    valid = valid && length == TCP_HEADER_MIN &&
        !tcp_receive_ipv6(&tcp6, destination6, source6,
                          segment, (u32)length, &ack6) &&
        ack6.valid && ack6.flags == TCP_FLAG_ACK;
    u32 state6;
    valid = valid && !tcp_connection_state(&tcp6, connection6, &state6) &&
        state6 == TCP_STATE_ESTABLISHED;
    length = tcp_build_ipv6(
        segment, sizeof(segment), destination6, source6,
        8081, 51000, 7001, syn6.sequence + 1,
        TCP_FLAG_ACK | TCP_FLAG_PSH, 65535,
        payload, sizeof(payload));
    valid = valid && !tcp_receive_ipv6(
        &tcp6, destination6, source6,
        segment, (u32)length, &ack6) && ack6.valid;
    received_length = 0;
    valid = valid && !tcp_receive_data(
        &tcp6, connection6, received, sizeof(received), &received_length) &&
        received_length == sizeof(payload);
    static struct tcp_context passive6;
    tcp_init(&passive6, 0x60006000u);
    u64 listener6 = tcp_listen_ipv6(&passive6, destination6, 8082);
    length = tcp_build_ipv6(
        segment, sizeof(segment), source6, destination6,
        52000, 8082, 1234, 0, TCP_FLAG_SYN, 65535, 0, 0);
    struct tcp_response synack6;
    valid = valid && listener6 && length == TCP_HEADER_MIN &&
        !tcp_receive_ipv6(&passive6, source6, destination6,
                          segment, (u32)length, &synack6) && synack6.valid &&
        synack6.flags == (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
        synack6.acknowledgement == 1235;
    length = tcp_build_ipv6(
        segment, sizeof(segment), source6, destination6,
        52000, 8082, 1235, synack6.sequence + 1,
        TCP_FLAG_ACK, 65535, 0, 0);
    valid = valid && length == TCP_HEADER_MIN &&
        !tcp_receive_ipv6(&passive6, source6, destination6,
                          segment, (u32)length, &ack6);
    u64 accepted6 = 0;
    valid = valid && !tcp_accept(&passive6, listener6, &accepted6) && accepted6;
    valid = valid && !tcp_connection_state(
        &passive6, accepted6, &state6) && state6 == TCP_STATE_ESTABLISHED;
    static struct tcp_context stress;
    tcp_init(&stress, 0x12345678u);
    u32 stress_sequences[3] = {0, 0, 0};
    for (u32 cycle = 0; cycle < 1024 && valid; cycle++) {
        u64 connection = tcp_active_open(
            &stress, client_address, (u16)(40000 + (cycle & 1023)),
            server_address, 8080);
        u32 state;
        if (connection && cycle < 3)
            stress_sequences[cycle] =
                stress.connections[(u32)connection - 1].send_unacknowledged;
        if (!connection || tcp_connection_state(&stress, connection, &state) ||
            state != TCP_STATE_SYN_SENT || tcp_close(&stress, connection) ||
            !tcp_connection_state(&stress, connection, &state))
            valid = 0;
    }
    valid = valid && stress_sequences[0] != stress_sequences[1] &&
        stress_sequences[1] != stress_sequences[2] &&
        stress_sequences[1] - stress_sequences[0] != 0x10001u &&
        stress_sequences[2] - stress_sequences[1] != 0x10001u;
    static struct tcp_context timeout;
    tcp_init(&timeout, 77);
    u64 timed = tcp_active_open(
        &timeout, client_address, 45000, server_address, 8080);
    struct tcp_transmit timed_transmit;
    valid = valid && timed &&
        tcp_prepare_transmit(&timeout, timed, 0, &timed_transmit) == 1;
    u32 now = TCP_RTO_INITIAL;
    for (u32 retry = 0; retry <= TCP_RETRY_MAX && valid; retry++) {
        int ticked = tcp_tick(&timeout, now, &timed_transmit);
        if (retry < TCP_RETRY_MAX && ticked != 1) valid = 0;
        now += TCP_RTO_MAX;
    }
    i32 timeout_error = 0;
    valid = valid && timeout.stats.retransmission_failures == 1 &&
        !timeout.stats.pmtu_blackholes &&
        !tcp_take_error(&timeout, timed, &timeout_error) &&
        timeout_error == -110 &&
        !tcp_take_error(&timeout, timed, &timeout_error) &&
        timeout_error == 0;
    static struct tcp_context persist;
    tcp_init(&persist, 500);
    u64 persist_id = tcp_active_open(
        &persist, client_address, 46000, server_address, 8080);
    struct tcp_connection *persist_connection =
        persist_id ? &persist.connections[(u32)persist_id - 1] : 0;
    if (persist_connection) {
        persist_connection->state = TCP_STATE_ESTABLISHED;
        persist_connection->send_unacknowledged = 501;
        persist_connection->send_next = 501;
        persist_connection->send_window = 0;
        persist_connection->receive_next = 700;
    }
    valid = valid && persist_connection &&
        !tcp_queue_send(&persist, persist_id, payload, sizeof(payload)) &&
        tcp_prepare_transmit(&persist, persist_id, 100,
                             &timed_transmit) == 0 &&
        tcp_tick(&persist, 199, &timed_transmit) == 0 &&
        tcp_tick(&persist, 200, &timed_transmit) == 1 &&
        timed_transmit.length == 1 && timed_transmit.sequence == 501 &&
        persist.stats.persist_probes == 1;
    if (persist_connection) {
        persist_connection->send_buffer_length = 0;
        persist_connection->send_buffer_offset = 0;
    }
    valid = valid && tcp_detach(
        &persist, persist_id, 201, &timed_transmit) == 1 &&
        timed_transmit.flags == (TCP_FLAG_FIN | TCP_FLAG_ACK) &&
        persist_connection->state == TCP_STATE_FIN_WAIT_1 &&
        persist_connection->detached;
    static struct tcp_context flood;
    tcp_init(&flood, 900);
    u64 flood_listener = tcp_listen(&flood, server_address, 9090);
    valid = valid && flood_listener &&
        !tcp_set_listener_backlog(&flood, flood_listener, 8);
    for (u32 index = 0; index < 96; index++) {
        length = tcp_build_ipv4(
            segment, sizeof(segment), client_address + index + 1,
            server_address, (u16)(30000 + index), 9090,
            100 + index, 0, TCP_FLAG_SYN, 65535, 0, 0);
        tcp_receive_ipv4(&flood, client_address + index + 1,
                         server_address, segment, (u32)length, &none);
    }
    valid = valid && flood.stats.syn_drops == 88;
    u8 mutation[64];
    for (u32 iteration = 0; iteration < 4096; iteration++) {
        for (u32 index = 0; index < sizeof(mutation); index++) mutation[index] = 0;
        mutation[0] = 0x30;
        mutation[1] = 0x39;
        mutation[2] = 0x1F;
        mutation[3] = 0x90;
        mutation[12] = 5 << 4;
        mutation[13] = TCP_FLAG_ACK;
        u32 position = iteration % sizeof(mutation);
        mutation[position] ^= (u8)(1u << (iteration & 7));
        tcp_parse(mutation, (iteration % 45) + 20, &view);
    }
        //
    // Phase 3: options round-trip (MSS + SACK-permitted through the
    // opts builders, IPv4 and IPv6).
    //
    u8 sack_syn_opts[8] = {2, 4, 0x05, 0xB4, 1, 1, 4, 2};
    u8 optseg[96];
    int optlen = tcp_build_ipv4_opts(
        optseg, sizeof(optseg), client_address, server_address,
        1, 2, 100, 0, TCP_FLAG_SYN, 65535, sack_syn_opts, 8, 0, 0);
    valid = valid && optlen == 28 &&
        !tcp_parse(optseg, (u32)optlen, &view) &&
        view.header_length == 28 && view.mss == 1460 && view.sack_permitted;
    optlen = tcp_build_ipv6_opts(
        optseg, sizeof(optseg), source6, destination6,
        1, 2, 100, 0, TCP_FLAG_SYN, 65535, sack_syn_opts, 8, 0, 0);
    valid = valid && optlen == 28 &&
        !tcp_checksum_ipv6(source6, destination6, optseg, (u32)optlen) &&
        !tcp_parse(optseg, (u32)optlen, &view) &&
        view.header_length == 28 && view.mss == 1460 && view.sack_permitted;
        //
    // Phase 3: SACK end-to-end. The client advertises SACK-permitted in
    // its SYN, the server echoes it in the SYN-ACK, SACK blocks are
    // derived from the OOO buffer, they retire in-flight entries on the
    // sender, and dupACK-3 fast retransmits the SACK hole.
    //
    static struct tcp_context sack_client;
    static struct tcp_context sack_server;
    tcp_init(&sack_client, 3001);
    tcp_init(&sack_server, 3002);
    u64 sack_listener = tcp_listen(&sack_server, server_address, 9999);
    u64 sack_id = tcp_active_open(&sack_client, client_address, 60000,
                                  server_address, 9999);
    valid = valid && sack_listener && sack_id;
    u32 sack_isn = sack_id
        ? sack_client.connections[(u32)sack_id - 1].send_unacknowledged : 0;
    u8 sseg[128];
    int slen = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), client_address, server_address,
        60000, 9999, sack_isn, 0, TCP_FLAG_SYN, 65535,
        sack_syn_opts, 8, 0, 0);
    struct tcp_response ssynack;
    valid = valid && slen == 28 &&
        !tcp_receive_ipv4(&sack_server, client_address, server_address,
                          sseg, (u32)slen, &ssynack) &&
        ssynack.valid && ssynack.flags == (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
        ssynack.option_length == 8 &&
        ssynack.options[2] == (u8)(TCP_RETRANSMIT_DATA_MAX >> 8) &&
        ssynack.options[3] == (u8)TCP_RETRANSMIT_DATA_MAX;
    int clen = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), server_address, client_address,
        9999, 60000, ssynack.sequence, ssynack.acknowledgement,
        ssynack.flags, ssynack.window,
        ssynack.options, ssynack.option_length, 0, 0);
    struct tcp_response ssock;
    struct tcp_connection *sack_connection = 0;
    if (clen > 0)
        sack_connection = sack_id
            ? &sack_client.connections[(u32)sack_id - 1] : 0;
    valid = valid && clen == 28 &&
        !tcp_receive_ipv4(&sack_client, server_address, client_address,
                          sseg, (u32)clen, &ssock) &&
        ssock.valid && sack_connection &&
        sack_connection->state == TCP_STATE_ESTABLISHED &&
        sack_connection->peer_sack &&
        sack_connection->remote_mss == TCP_RETRANSMIT_DATA_MAX;
    int sack_handshake_ack = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), client_address, server_address,
        60000, 9999, ssock.sequence, ssock.acknowledgement,
        ssock.flags, ssock.window, 0, 0, 0, 0);
    struct tcp_response sfinal;
    valid = valid && sack_handshake_ack == TCP_HEADER_MIN &&
        !tcp_receive_ipv4(&sack_server, client_address, server_address,
                          sseg, (u32)sack_handshake_ack, &sfinal);
    struct tcp_connection *sack_accepted = 0;
    for (u32 index = 0; index < TCP_CONNECTION_MAX; index++)
        if (sack_server.connections[index].active &&
            sack_server.connections[index].state == TCP_STATE_ESTABLISHED)
            sack_accepted = &sack_server.connections[index];
    valid = valid && sack_accepted && sack_accepted->peer_sack;
    u32 sack_base = sack_connection->send_unacknowledged;
    u8 sp1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    u8 sp2[8] = {9, 10, 11, 12, 13, 14, 15, 16};
    u8 sp3[8] = {17, 18, 19, 20, 21, 22, 23, 24};
    struct tcp_transmit st1, st2, st3;
    valid = valid && !tcp_queue_send(&sack_client, sack_id, sp1, 8) &&
        tcp_prepare_transmit(&sack_client, sack_id, 100, &st1) == 1 &&
        st1.sequence == sack_base;
    int s1len = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), client_address, server_address,
        60000, 9999, st1.sequence, st1.acknowledgement,
        st1.flags, 65535, 0, 0, st1.data, st1.length);
    struct tcp_response sr1;
    valid = valid && s1len == TCP_HEADER_MIN + 8 &&
        !tcp_receive_ipv4(&sack_server, client_address, server_address,
                          sseg, (u32)s1len, &sr1) && sr1.valid &&
        sr1.acknowledgement == sack_base + 8;
    int s1ack = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), server_address, client_address,
        9999, 60000, sr1.sequence, sr1.acknowledgement,
        sr1.flags, sr1.window, 0, 0, 0, 0);
    valid = valid && s1ack == TCP_HEADER_MIN &&
        !tcp_receive_ipv4(&sack_client, server_address, client_address,
                          sseg, (u32)s1ack, &ssock);
    valid = valid && !tcp_queue_send(&sack_client, sack_id, sp2, 8) &&
        tcp_prepare_transmit(&sack_client, sack_id, 110, &st2) == 1 &&
        st2.sequence == sack_base + 8;
    valid = valid && !tcp_queue_send(&sack_client, sack_id, sp3, 8) &&
        tcp_prepare_transmit(&sack_client, sack_id, 120, &st3) == 1 &&
        st3.sequence == sack_base + 16;
    int s3len = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), client_address, server_address,
        60000, 9999, st3.sequence, st3.acknowledgement,
        st3.flags, 65535, 0, 0, st3.data, st3.length);
    struct tcp_response sr3;
    valid = valid && s3len == TCP_HEADER_MIN + 8 &&
        !tcp_receive_ipv4(&sack_server, client_address, server_address,
                          sseg, (u32)s3len, &sr3) &&
        sr3.valid && sr3.sack_length == 12 &&
        sack_server.stats.out_of_order_queued == 1;
    int a3len = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), server_address, client_address,
        9999, 60000, sr3.sequence, sr3.acknowledgement,
        sr3.flags, sr3.window, sr3.sack, sr3.sack_length, 0, 0);
    valid = valid && a3len > 0 &&
        !tcp_receive_ipv4(&sack_client, server_address, client_address,
                          sseg, (u32)a3len, &ssock) &&
        sack_client.stats.sack_retired_segments == 1 &&
        sack_client.stats.sack_blocks_received >= 1 &&
        sack_client.stats.duplicate_acknowledgements == 1;
    for (u32 dup = 0; dup < 2; dup++) {
        valid = valid &&
            !tcp_receive_ipv4(&sack_client, server_address, client_address,
                              sseg, (u32)a3len, &ssock);
    }
    valid = valid &&
        sack_client.stats.duplicate_acknowledgements == 3 &&
        sack_client.stats.fast_retransmits == 1 &&
        sack_client.stats.sack_fast_retransmits == 1;
    struct tcp_transmit sack_retransmit;
    valid = valid &&
        tcp_tick(&sack_client, 120, &sack_retransmit) == 1 &&
        sack_retransmit.retransmission &&
        sack_retransmit.sequence == sack_base + 8 &&
        sack_retransmit.length == 8;
    int s2len = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), client_address, server_address,
        60000, 9999, st2.sequence, st2.acknowledgement,
        st2.flags, 65535, 0, 0, sp2, 8);
    struct tcp_response sr2;
    valid = valid && s2len == TCP_HEADER_MIN + 8 &&
        !tcp_receive_ipv4(&sack_server, client_address, server_address,
                          sseg, (u32)s2len, &sr2) &&
        sr2.valid && sr2.sack_length == 0 &&
        sack_accepted->receive_next == sack_base + 24;
    u8 sack_recv[32];
    u32 sack_recv_len = 0;
    u64 sack_accepted_id = 0;
    for (u32 index = 0; index < TCP_CONNECTION_MAX; index++)
        if (sack_accepted == &sack_server.connections[index])
            sack_accepted_id = ((u64)sack_accepted->generation << 32) |
                (index + 1);
    valid = valid && !tcp_receive_data(&sack_server, sack_accepted_id,
                                       sack_recv, sizeof(sack_recv),
                                       &sack_recv_len) &&
        sack_recv_len == 24 && sack_recv[0] == 1 && sack_recv[8] == 9 &&
        sack_recv[16] == 17;
        //
    // Phase 3: SACK block coalescing (adjacent OOO segments merge into
    // one block), duplicate SACK detection, and two independent blocks.
    //
    u32 coalesce_base = sack_accepted->receive_next;
    u8 cp1[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    u8 cp2[8] = {2, 2, 2, 2, 2, 2, 2, 2};
    u8 cp3[8] = {3, 3, 3, 3, 3, 3, 3, 3};
    int co1 = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), client_address, server_address,
        60000, 9999, coalesce_base + 8, 0,
        TCP_FLAG_ACK | TCP_FLAG_PSH, 65535, 0, 0, cp1, 8);
    struct tcp_response cr1;
    valid = valid && co1 == TCP_HEADER_MIN + 8 &&
        !tcp_receive_ipv4(&sack_server, client_address, server_address,
                          sseg, (u32)co1, &cr1) &&
        cr1.valid && cr1.sack_length == 12;
    struct tcp_response crdup;
    valid = valid &&
        !tcp_receive_ipv4(&sack_server, client_address, server_address,
                          sseg, (u32)co1, &crdup) &&
        crdup.valid && crdup.sack_length == 12 &&
        sack_server.stats.sack_duplicate_acks == 1 &&
        sack_server.stats.out_of_order_queued == 2;
    int co2 = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), client_address, server_address,
        60000, 9999, coalesce_base + 16, 0,
        TCP_FLAG_ACK | TCP_FLAG_PSH, 65535, 0, 0, cp2, 8);
    struct tcp_response cr2;
    valid = valid && co2 == TCP_HEADER_MIN + 8 &&
        !tcp_receive_ipv4(&sack_server, client_address, server_address,
                          sseg, (u32)co2, &cr2) &&
        cr2.valid && cr2.sack_length == 12;
    int co3 = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), client_address, server_address,
        60000, 9999, coalesce_base + 32, 0,
        TCP_FLAG_ACK | TCP_FLAG_PSH, 65535, 0, 0, cp3, 8);
    struct tcp_response cr3;
    valid = valid && co3 == TCP_HEADER_MIN + 8 &&
        !tcp_receive_ipv4(&sack_server, client_address, server_address,
                          sseg, (u32)co3, &cr3) &&
        cr3.valid && cr3.sack_length == 20;
    int cplen = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), server_address, client_address,
        9999, 60000, cr3.sequence, cr3.acknowledgement,
        cr3.flags, cr3.window, cr3.sack, cr3.sack_length, 0, 0);
    valid = valid && cplen > 0 &&
        !tcp_parse(sseg, (u32)cplen, &view) &&
        view.sack_block_count == 2 &&
        view.sack_blocks[0][0] == coalesce_base + 8 &&
        view.sack_blocks[0][1] == coalesce_base + 24 &&
        view.sack_blocks[1][0] == coalesce_base + 32 &&
        view.sack_blocks[1][1] == coalesce_base + 40;
        //
    // Phase 3: BBR v1. Drive eight 512-byte segments with a 10-tick RTT;
    // the ACK clock must converge on 51.2 B/ms, min RTT 10, and the
    // phase machine must walk STARTUP -> DRAIN -> PROBE_BW with the
    // cwnd clamped to BDP (floored at 4 MSS).
    //
    static struct tcp_context bbr_ctx;
    tcp_init(&bbr_ctx, 4242);
    u64 bbr_id = tcp_active_open(&bbr_ctx, client_address, 61000,
                                 server_address, 9001);
    struct tcp_connection *bbr_connection = bbr_id
        ? &bbr_ctx.connections[(u32)bbr_id - 1] : 0;
    if (bbr_connection) {
        bbr_connection->state = TCP_STATE_ESTABLISHED;
        bbr_connection->send_unacknowledged = 1000;
        bbr_connection->send_next = 1000;
        bbr_connection->receive_next = 2000;
    }
    valid = valid && bbr_id && bbr_connection;
    u8 bpayload[512];
    for (u32 index = 0; index < sizeof(bpayload); index++)
        bpayload[index] = (u8)index;
    struct tcp_transmit btransmit;
    for (u32 round = 0; round < 8 && valid; round++) {
        u32 now = (u32)(100 + round * 100);
        valid = valid && !tcp_queue_send(&bbr_ctx, bbr_id, bpayload, 512) &&
            tcp_prepare_transmit(&bbr_ctx, bbr_id, now, &btransmit) == 1;
        tcp_tick(&bbr_ctx, now + 10, &btransmit);
        int blen = tcp_build_ipv4_opts(
            sseg, sizeof(sseg), server_address, client_address,
            9001, 61000, bbr_connection->receive_next,
            bbr_connection->send_next, TCP_FLAG_ACK, 65535, 0, 0, 0, 0);
        struct tcp_response br;
        valid = valid && blen == TCP_HEADER_MIN &&
            !tcp_receive_ipv4(&bbr_ctx, server_address, client_address,
                              sseg, (u32)blen, &br);
    }
    u32 bbr_cwnd = 0, bbr_bw = 0, bbr_min_rtt = 0, bbr_phase = 99;
    valid = valid && !tcp_cc_debug(&bbr_ctx, bbr_id,
                                   &bbr_cwnd, &bbr_bw, &bbr_min_rtt,
                                   &bbr_phase) &&
        bbr_min_rtt == 10 &&
        bbr_bw == (u32)(((u64)512 * 65536) / 100) &&
                // Round 7's gain slot is 1.25: 4 MSS * 1280 >> 10 = 5 MSS.
        bbr_phase == TCP_BBR_PROBE_BW &&
        bbr_cwnd == TCP_RETRANSMIT_DATA_MAX * 5;
    valid = valid && !tcp_queue_send(&bbr_ctx, bbr_id, bpayload, 512) &&
        tcp_prepare_transmit(&bbr_ctx, bbr_id, 1000, &btransmit) == 1;
    u32 bbr_deadline = 0;
    for (u32 index = 0; index < TCP_RETRANSMISSION_MAX; index++)
        if (bbr_connection->retransmissions[index].active)
            bbr_deadline = bbr_connection->retransmissions[index].deadline;
    struct tcp_transmit bbr_rto;
    valid = valid && bbr_deadline &&
        tcp_tick(&bbr_ctx, bbr_deadline, &bbr_rto) == 1 &&
        bbr_rto.retransmission;
    valid = valid && !tcp_cc_debug(&bbr_ctx, bbr_id,
                                   &bbr_cwnd, &bbr_bw, &bbr_min_rtt,
                                   &bbr_phase) &&
                // Fresh model: cwnd = max(BDP/2, 4 MSS); BDP floored at 4 MSS.
        bbr_cwnd == TCP_RETRANSMIT_DATA_MAX * 4 &&
        bbr_phase == TCP_BBR_PROBE_BW;
        //
    // Phase 3: Reno A/B contrast on the identical traffic shape:
    // slow start adds the acked bytes per ACK.
    //
    static struct tcp_context reno_ctx;
    tcp_init(&reno_ctx, 5678);
    tcp_set_cc(&reno_ctx, tcp_cc_reno());
    u64 reno_id = tcp_active_open(&reno_ctx, client_address, 61001,
                                  server_address, 9001);
    struct tcp_connection *reno_connection = reno_id
        ? &reno_ctx.connections[(u32)reno_id - 1] : 0;
    if (reno_connection) {
        reno_connection->state = TCP_STATE_ESTABLISHED;
        reno_connection->send_unacknowledged = 1000;
        reno_connection->send_next = 1000;
        reno_connection->receive_next = 2000;
    }
    valid = valid && reno_id && reno_connection;
    for (u32 round = 0; round < 8 && valid; round++) {
        u32 now = (u32)(100 + round * 100);
        valid = valid && !tcp_queue_send(&reno_ctx, reno_id, bpayload, 512) &&
            tcp_prepare_transmit(&reno_ctx, reno_id, now, &btransmit) == 1;
        tcp_tick(&reno_ctx, now + 10, &btransmit);
        int rlen = tcp_build_ipv4_opts(
            sseg, sizeof(sseg), server_address, client_address,
            9001, 61001, reno_connection->receive_next,
            reno_connection->send_next, TCP_FLAG_ACK, 65535, 0, 0, 0, 0);
        struct tcp_response rr;
        valid = valid && rlen == TCP_HEADER_MIN &&
            !tcp_receive_ipv4(&reno_ctx, server_address, client_address,
                              sseg, (u32)rlen, &rr);
    }
    valid = valid && reno_connection->congestion_window ==
        TCP_RETRANSMIT_DATA_MAX * 2 + 512 * 8;
        //
    // Phase 3: ACK filtering. The first in-order data ACK goes out
    // immediately and opens a one-RTT suppression window; data inside
    // the window only raises ack_pending, and the deadline heap flushes
    // the combined ACK.
    //
    static struct tcp_context af_ctx;
    tcp_init(&af_ctx, 777);
    tcp_set_ack_filter(&af_ctx, 1);
    u64 af_id = tcp_active_open(&af_ctx, client_address, 62000,
                                server_address, 9002);
    struct tcp_connection *af_connection = af_id
        ? &af_ctx.connections[(u32)af_id - 1] : 0;
    if (af_connection) {
        af_connection->state = TCP_STATE_ESTABLISHED;
        af_connection->send_unacknowledged = 500;
        af_connection->send_next = 500;
        af_connection->receive_next = 900;
    }
    valid = valid && af_id && af_connection;
    u8 afp1[8] = {7, 7, 7, 7, 7, 7, 7, 7};
    u8 afp2[8] = {8, 8, 8, 8, 8, 8, 8, 8};
    u8 afp3[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    struct tcp_transmit af_transmit;
    tcp_tick(&af_ctx, 1000, &af_transmit);
    int af1 = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), server_address, client_address,
        9002, 62000, 900, 0, TCP_FLAG_ACK | TCP_FLAG_PSH, 65535,
        0, 0, afp1, 8);
    struct tcp_response ar1;
    valid = valid && af1 == TCP_HEADER_MIN + 8 &&
        !tcp_receive_ipv4(&af_ctx, server_address, client_address,
                          sseg, (u32)af1, &ar1) &&
        ar1.valid && ar1.acknowledgement == 908 &&
        af_ctx.stats.acks_sent == 1;
    tcp_tick(&af_ctx, 1005, &af_transmit);
    int af2 = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), server_address, client_address,
        9002, 62000, 908, 0, TCP_FLAG_ACK | TCP_FLAG_PSH, 65535,
        0, 0, afp2, 8);
    struct tcp_response ar2;
    valid = valid && af2 == TCP_HEADER_MIN + 8 &&
        !tcp_receive_ipv4(&af_ctx, server_address, client_address,
                          sseg, (u32)af2, &ar2) &&
        !ar2.valid && af_ctx.stats.acks_filtered == 1 &&
        af_connection->receive_next == 916;
    valid = valid &&
        tcp_tick(&af_ctx, 1099, &af_transmit) == 0 &&
        tcp_tick(&af_ctx, 1100, &af_transmit) == 1 &&
        af_transmit.flags == TCP_FLAG_ACK &&
        af_transmit.acknowledgement == 916 &&
        af_ctx.stats.acks_sent == 2;
    tcp_tick(&af_ctx, 1105, &af_transmit);
    int af3 = tcp_build_ipv4_opts(
        sseg, sizeof(sseg), server_address, client_address,
        9002, 62000, 916, 0, TCP_FLAG_ACK | TCP_FLAG_PSH, 65535,
        0, 0, afp3, 8);
    struct tcp_response ar3;
    valid = valid && af3 == TCP_HEADER_MIN + 8 &&
        !tcp_receive_ipv4(&af_ctx, server_address, client_address,
                          sseg, (u32)af3, &ar3) &&
        ar3.valid && af_ctx.stats.acks_sent == 3;
        //
    // Phase 3: the global deadline heap. Events fire in deadline order
    // across connections; closing a connection invalidates its pending
    // events via the timer version token.
    //
    static struct tcp_context heap_ctx;
    tcp_init(&heap_ctx, 555);
    u64 h1 = tcp_active_open(&heap_ctx, client_address, 63000,
                             server_address, 9003);
    u64 h2 = tcp_active_open(&heap_ctx, client_address, 63001,
                             server_address, 9003);
    u64 h3 = tcp_active_open(&heap_ctx, client_address, 63002,
                             server_address, 9003);
    struct tcp_connection *hc1 = h1 ? &heap_ctx.connections[(u32)h1 - 1] : 0;
    struct tcp_connection *hc2 = h2 ? &heap_ctx.connections[(u32)h2 - 1] : 0;
    struct tcp_connection *hc3 = h3 ? &heap_ctx.connections[(u32)h3 - 1] : 0;
    if (hc1) {
        hc1->state = TCP_STATE_ESTABLISHED;
        hc1->send_unacknowledged = 1; hc1->send_next = 1;
        hc1->receive_next = 1;
    }
    if (hc2) {
        hc2->state = TCP_STATE_ESTABLISHED;
        hc2->send_unacknowledged = 1; hc2->send_next = 1;
        hc2->receive_next = 1;
    }
    if (hc3) {
        hc3->state = TCP_STATE_ESTABLISHED;
        hc3->send_unacknowledged = 1; hc3->send_next = 1;
        hc3->receive_next = 1;
    }
    valid = valid && h1 && h2 && h3 &&
        !tcp_queue_send(&heap_ctx, h1, sp1, 8) &&
        !tcp_queue_send(&heap_ctx, h2, sp2, 8) &&
        !tcp_queue_send(&heap_ctx, h3, sp3, 8);
    struct tcp_transmit heap_transmit;
    valid = valid &&
        tcp_prepare_transmit(&heap_ctx, h1, 10, &heap_transmit) == 1 &&
        tcp_prepare_transmit(&heap_ctx, h2, 150, &heap_transmit) == 1 &&
        tcp_prepare_transmit(&heap_ctx, h3, 300, &heap_transmit) == 1;
    valid = valid &&
        tcp_tick(&heap_ctx, 109, &heap_transmit) == 0 &&
        tcp_tick(&heap_ctx, 110, &heap_transmit) == 1 &&
        heap_transmit.retransmission &&
        heap_transmit.connection_id == h1;
    valid = valid &&
        tcp_tick(&heap_ctx, 249, &heap_transmit) == 0 &&
        tcp_tick(&heap_ctx, 250, &heap_transmit) == 1 &&
        heap_transmit.connection_id == h2;
    valid = valid && !tcp_close(&heap_ctx, h3) &&
        !tcp_close(&heap_ctx, h1) &&
        tcp_tick(&heap_ctx, 400, &heap_transmit) == 0 &&
        tcp_tick(&heap_ctx, 500, &heap_transmit) == 1 &&
        heap_transmit.connection_id == h2;
    struct tcp_transmit optsz;
    valid = valid && sizeof(optsz.options) >= 20;
    static struct tcp_context close_ctx;
    tcp_init(&close_ctx, 8800);
    u64 close_id = tcp_active_open(&close_ctx, client_address, 64000,
                                   server_address, 9100);
    struct tcp_connection *close_c = close_id
        ? &close_ctx.connections[(u32)close_id - 1] : 0;
    if (close_c) {
        close_c->state = TCP_STATE_ESTABLISHED;
        close_c->send_unacknowledged = 10;
        close_c->send_next = 10;
        close_c->receive_next = 20;
        close_c->send_window = 0;
    }
    valid = valid && close_c &&
        !tcp_queue_send(&close_ctx, close_id, sp1, 8);
    struct tcp_transmit close_tx;
    valid = valid && tcp_shutdown(&close_ctx, close_id, 10, &close_tx) == 1 &&
        close_c->state == TCP_STATE_ESTABLISHED && close_c->send_fin;
    valid = valid && tcp_detach(&close_ctx, close_id, 11, &close_tx) == 1 &&
        close_tx.flags == (TCP_FLAG_RST | TCP_FLAG_ACK);
    static struct tcp_context sim_client;
    static struct tcp_context sim_server;
    tcp_init(&sim_client, 8811);
    tcp_init(&sim_server, 8812);
    u64 sim_listener = tcp_listen(&sim_server, server_address, 9102);
    u64 sim_id = tcp_active_open(&sim_client, client_address, 64002,
                                 server_address, 9102);
    valid = valid && sim_listener && sim_id;
    u32 sim_isn = sim_client.connections[(u32)sim_id - 1].send_unacknowledged;
    int simlen = tcp_build_ipv4(
        segment, sizeof(segment), client_address, server_address,
        64002, 9102, sim_isn, 0, TCP_FLAG_SYN, 65535, 0, 0);
    struct tcp_response sim_synack;
    valid = valid && simlen == TCP_HEADER_MIN &&
        !tcp_receive_ipv4(&sim_server, client_address, server_address,
                          segment, (u32)simlen, &sim_synack) && sim_synack.valid;
    simlen = tcp_build_ipv4(
        segment, sizeof(segment), server_address, client_address,
        9102, 64002, sim_synack.sequence, sim_synack.acknowledgement,
        TCP_FLAG_SYN | TCP_FLAG_ACK, 65535, 0, 0);
    struct tcp_response sim_ack;
    valid = valid && !tcp_receive_ipv4(
        &sim_client, server_address, client_address,
        segment, (u32)simlen, &sim_ack) && sim_ack.valid;
    simlen = tcp_build_ipv4(
        segment, sizeof(segment), client_address, server_address,
        64002, 9102, sim_ack.sequence, sim_ack.acknowledgement,
        TCP_FLAG_ACK, 65535, 0, 0);
    valid = valid && !tcp_receive_ipv4(
        &sim_server, client_address, server_address,
        segment, (u32)simlen, &none);
    u64 sim_accepted = 0;
    valid = valid && !tcp_accept(&sim_server, sim_listener, &sim_accepted);
    struct tcp_transmit sim_cfin;
    struct tcp_transmit sim_sfin;
    valid = valid && !tcp_shutdown(&sim_client, sim_id, 50, &sim_cfin) &&
        (sim_cfin.flags & TCP_FLAG_FIN);
    valid = valid && !tcp_shutdown(&sim_server, sim_accepted, 51, &sim_sfin) &&
        (sim_sfin.flags & TCP_FLAG_FIN);
    simlen = tcp_build_ipv4(
        segment, sizeof(segment), server_address, client_address,
        9102, 64002, sim_sfin.sequence, sim_sfin.acknowledgement,
        sim_sfin.flags, sim_sfin.window, 0, 0);
    struct tcp_response sim_peer;
    valid = valid && !tcp_receive_ipv4(
        &sim_client, server_address, client_address,
        segment, (u32)simlen, &sim_peer) && sim_peer.valid;
    u32 sim_state = 0;
    valid = valid && !tcp_connection_state(&sim_client, sim_id, &sim_state) &&
        sim_state == TCP_STATE_CLOSING;
    simlen = tcp_build_ipv4(
        segment, sizeof(segment), server_address, client_address,
        9102, 64002, sim_sfin.sequence + 1, sim_cfin.sequence + 1,
        TCP_FLAG_ACK, 65535, 0, 0);
    valid = valid && !tcp_receive_ipv4(
        &sim_client, server_address, client_address,
        segment, (u32)simlen, &none);
    valid = valid && !tcp_connection_state(&sim_client, sim_id, &sim_state) &&
        sim_state == TCP_STATE_TIME_WAIT;
    static struct tcp_context fin_client;
    static struct tcp_context fin_server;
    tcp_init(&fin_client, 8801);
    tcp_init(&fin_server, 8802);
    u64 fin_listener = tcp_listen(&fin_server, server_address, 9101);
    u64 fin_id = tcp_active_open(&fin_client, client_address, 64001,
                                 server_address, 9101);
    valid = valid && fin_listener && fin_id;
    u32 fin_isn = fin_client.connections[(u32)fin_id - 1].send_unacknowledged;
    int flen = tcp_build_ipv4(
        segment, sizeof(segment), client_address, server_address,
        64001, 9101, fin_isn, 0, TCP_FLAG_SYN, 65535, 0, 0);
    struct tcp_response fin_synack;
    valid = valid && flen == TCP_HEADER_MIN &&
        !tcp_receive_ipv4(&fin_server, client_address, server_address,
                          segment, (u32)flen, &fin_synack) && fin_synack.valid;
    flen = tcp_build_ipv4(
        segment, sizeof(segment), server_address, client_address,
        9101, 64001, fin_synack.sequence, fin_synack.acknowledgement,
        TCP_FLAG_SYN | TCP_FLAG_ACK, 65535, 0, 0);
    struct tcp_response fin_ack;
    valid = valid && !tcp_receive_ipv4(
        &fin_client, server_address, client_address,
        segment, (u32)flen, &fin_ack) && fin_ack.valid;
    flen = tcp_build_ipv4(
        segment, sizeof(segment), client_address, server_address,
        64001, 9101, fin_ack.sequence, fin_ack.acknowledgement,
        TCP_FLAG_ACK, 65535, 0, 0);
    valid = valid && !tcp_receive_ipv4(
        &fin_server, client_address, server_address,
        segment, (u32)flen, &none);
    u64 fin_accepted = 0;
    valid = valid && !tcp_accept(&fin_server, fin_listener, &fin_accepted);
    struct tcp_transmit fin_tx;
    valid = valid && !tcp_shutdown(&fin_client, fin_id, 30, &fin_tx) &&
        (fin_tx.flags & TCP_FLAG_FIN);
    u32 fin_state = 0;
    valid = valid && !tcp_connection_state(&fin_client, fin_id, &fin_state) &&
        fin_state == TCP_STATE_FIN_WAIT_1;
    flen = tcp_build_ipv4(
        segment, sizeof(segment), client_address, server_address,
        64001, 9101, fin_tx.sequence, fin_tx.acknowledgement,
        fin_tx.flags, fin_tx.window, 0, 0);
    struct tcp_response fin_peer;
    valid = valid && !tcp_receive_ipv4(
        &fin_server, client_address, server_address,
        segment, (u32)flen, &fin_peer) && fin_peer.valid;
    u32 peer_state = 0;
    valid = valid && !tcp_connection_state(
        &fin_server, fin_accepted, &peer_state) &&
        peer_state == TCP_STATE_CLOSE_WAIT;
    flen = tcp_build_ipv4(
        segment, sizeof(segment), server_address, client_address,
        9101, 64001, fin_peer.sequence, fin_peer.acknowledgement,
        TCP_FLAG_ACK, 65535, 0, 0);
    valid = valid && !tcp_receive_ipv4(
        &fin_client, server_address, client_address,
        segment, (u32)flen, &none);
    valid = valid && !tcp_connection_state(&fin_client, fin_id, &fin_state) &&
        fin_state == TCP_STATE_FIN_WAIT_2;
    struct tcp_transmit last_tx;
    valid = valid && !tcp_shutdown(&fin_server, fin_accepted, 40, &last_tx) &&
        (last_tx.flags & TCP_FLAG_FIN);
    valid = valid && !tcp_connection_state(
        &fin_server, fin_accepted, &peer_state) &&
        peer_state == TCP_STATE_LAST_ACK;
    flen = tcp_build_ipv4(
        segment, sizeof(segment), server_address, client_address,
        9101, 64001, last_tx.sequence, last_tx.acknowledgement,
        last_tx.flags, last_tx.window, 0, 0);
    struct tcp_response fin_wait_ack;
    valid = valid && !tcp_receive_ipv4(
        &fin_client, server_address, client_address,
        segment, (u32)flen, &fin_wait_ack) && fin_wait_ack.valid;
    valid = valid && !tcp_connection_state(&fin_client, fin_id, &fin_state) &&
        fin_state == TCP_STATE_TIME_WAIT;
    flen = tcp_build_ipv4(
        segment, sizeof(segment), client_address, server_address,
        64001, 9101, fin_wait_ack.sequence, fin_wait_ack.acknowledgement,
        TCP_FLAG_ACK, 65535, 0, 0);
    valid = valid && !tcp_receive_ipv4(
        &fin_server, client_address, server_address,
        segment, (u32)flen, &none);
    valid = valid && tcp_connection_state(
        &fin_server, fin_accepted, &peer_state) < 0;
    struct tcp_transmit wait_tx;
    valid = valid &&
        tcp_tick(&fin_client, 30 + TCP_TIME_WAIT_TICKS - 1, &wait_tx) == 0 &&
        tcp_tick(&fin_client, 30 + TCP_TIME_WAIT_TICKS, &wait_tx) == 2 &&
        fin_client.stats.time_wait_expired == 1;
    struct tcp_transmit syn6_fin;
    valid = valid && !tcp_shutdown(&tcp6, connection6, 60, &syn6_fin) &&
        (syn6_fin.flags & TCP_FLAG_FIN);
    valid = valid && !tcp_connection_state(&tcp6, connection6, &state6) &&
        state6 == TCP_STATE_FIN_WAIT_1;
    length = tcp_build_ipv6(
        segment, sizeof(segment), destination6, source6,
        8081, 51000, 7001 + (u32)sizeof(payload), syn6_fin.sequence + 1,
        TCP_FLAG_FIN | TCP_FLAG_ACK, 65535, 0, 0);
    valid = valid && length == TCP_HEADER_MIN &&
        !tcp_receive_ipv6(&tcp6, destination6, source6,
                          segment, (u32)length, &ack6) && ack6.valid;
    valid = valid && !tcp_connection_state(&tcp6, connection6, &state6) &&
        state6 == TCP_STATE_TIME_WAIT;
    valid = valid && sack_connection &&
        !tcp_clamp_pmtu(&sack_client, 4, server_address, 0, 1280) &&
        sack_connection->remote_mss == 1240 &&
        tcp_clamp_pmtu(&sack_client, 4, server_address, 0, 1500) == 1 &&
        sack_connection->remote_mss == 1240;
        //
    // PMTU blackhole: two RTOs of a full-sized segment, no ICMP.
    // IPv4 drops 1500 -> 1280 -> 576; IPv6 floors at 1280. Small
    // payloads and SYNs must not look like a too-big packet.
    //
    static struct tcp_context hole;
    tcp_init(&hole, 9001);
    tcp_set_pmtu_blackhole_callback(&hole, hole_blackhole, 0);
    hole_mtu = 0;
    hole_family = 0;
    u64 hole_id = tcp_active_open(&hole, client_address, 47000,
                                  server_address, 8080);
    struct tcp_connection *hole_c = hole_id
        ? &hole.connections[(u32)hole_id - 1] : 0;
    if (hole_c) {
        hole_c->state = TCP_STATE_ESTABLISHED;
        hole_c->send_unacknowledged = 1000;
        hole_c->send_next = 1000;
        hole_c->receive_next = 2000;
    }
    static u8 hole_payload[TCP_RETRANSMIT_DATA_MAX];
    for (u32 index = 0; index < sizeof(hole_payload); index++)
        hole_payload[index] = (u8)index;
    struct tcp_transmit hole_tx;
    u32 hole_deadline;
    valid = valid && hole_c &&
        !tcp_queue_send(&hole, hole_id, hole_payload,
                        TCP_RETRANSMIT_DATA_MAX) &&
        tcp_prepare_transmit(&hole, hole_id, 10, &hole_tx) == 1 &&
        hole_tx.length == TCP_RETRANSMIT_DATA_MAX;
    hole_deadline = retransmission_deadline(hole_c);
    valid = valid && hole_deadline &&
        tcp_tick(&hole, hole_deadline, &hole_tx) == 1 &&
        hole_tx.retransmission &&
        hole_tx.length == TCP_RETRANSMIT_DATA_MAX &&
        hole_c->remote_mss == TCP_RETRANSMIT_DATA_MAX &&
        !hole.stats.pmtu_blackholes;
    hole_deadline = retransmission_deadline(hole_c);
    valid = valid && hole_deadline &&
        tcp_tick(&hole, hole_deadline, &hole_tx) == 1 &&
        hole_tx.length == TCP_PMTU_PLATEAU - TCP_PMTU_HEADER4 &&
        hole_c->remote_mss == TCP_PMTU_PLATEAU - TCP_PMTU_HEADER4 &&
        hole.stats.pmtu_blackholes == 1 &&
        hole_family == 4 && hole_mtu == TCP_PMTU_PLATEAU;
    valid = valid &&
        tcp_prepare_transmit(&hole, hole_id, hole_deadline, &hole_tx) == 1 &&
        hole_tx.length == TCP_RETRANSMIT_DATA_MAX -
            (TCP_PMTU_PLATEAU - TCP_PMTU_HEADER4);
    int hole_ack_len = tcp_build_ipv4(
        segment, sizeof(segment), server_address, client_address,
        8080, 47000, hole_c->receive_next, hole_c->send_next,
        TCP_FLAG_ACK, 65535, 0, 0);
    valid = valid && hole_ack_len == TCP_HEADER_MIN &&
        !tcp_receive_ipv4(&hole, server_address, client_address,
                          segment, (u32)hole_ack_len, &none) &&
        hole_c->send_unacknowledged == hole_c->send_next &&
        !hole_c->pmtu_blackhole_rtos;
    valid = valid &&
        !tcp_queue_send(&hole, hole_id, hole_payload,
                        TCP_PMTU_PLATEAU - TCP_PMTU_HEADER4) &&
        tcp_prepare_transmit(&hole, hole_id, hole_deadline + 10,
                             &hole_tx) == 1 &&
        hole_tx.length == TCP_PMTU_PLATEAU - TCP_PMTU_HEADER4;
    hole_deadline = retransmission_deadline(hole_c);
    valid = valid && hole_deadline &&
        tcp_tick(&hole, hole_deadline, &hole_tx) == 1 &&
        hole_c->remote_mss == TCP_PMTU_PLATEAU - TCP_PMTU_HEADER4 &&
        hole.stats.pmtu_blackholes == 1;
    hole_deadline = retransmission_deadline(hole_c);
    valid = valid && hole_deadline &&
        tcp_tick(&hole, hole_deadline, &hole_tx) == 1 &&
        hole_tx.length == TCP_PMTU_IPV4_MIN - TCP_PMTU_HEADER4 &&
        hole_c->remote_mss == TCP_PMTU_IPV4_MIN - TCP_PMTU_HEADER4 &&
        hole.stats.pmtu_blackholes == 2 &&
        hole_mtu == TCP_PMTU_IPV4_MIN &&
        tcp_clamp_pmtu(&hole, 4, server_address, 0, 1500) == 1 &&
        hole_c->remote_mss == TCP_PMTU_IPV4_MIN - TCP_PMTU_HEADER4;
    u64 tiny_id = tcp_active_open(&hole, client_address, 47001,
                                  server_address, 8080);
    struct tcp_connection *tiny_c = tiny_id
        ? &hole.connections[(u32)tiny_id - 1] : 0;
    if (tiny_c) {
        tiny_c->state = TCP_STATE_ESTABLISHED;
        tiny_c->send_unacknowledged = 1000;
        tiny_c->send_next = 1000;
        tiny_c->receive_next = 2000;
    }
    valid = valid && tiny_c &&
        !tcp_queue_send(&hole, tiny_id, payload, sizeof(payload)) &&
        tcp_prepare_transmit(&hole, tiny_id, 10, &hole_tx) == 1;
    hole_deadline = retransmission_deadline(tiny_c);
    valid = valid && hole_deadline &&
        tcp_tick(&hole, hole_deadline, &hole_tx) == 1;
    hole_deadline = retransmission_deadline(tiny_c);
    valid = valid && hole_deadline &&
        tcp_tick(&hole, hole_deadline, &hole_tx) == 1 &&
        tiny_c->remote_mss == TCP_RETRANSMIT_DATA_MAX &&
        hole.stats.pmtu_blackholes == 2;
    hole_mtu = 0;
    hole_family = 0;
    u64 hole6_id = tcp_active_open_ipv6(&hole, source6, 47002,
                                        destination6, 8080);
    struct tcp_connection *hole6_c = hole6_id
        ? &hole.connections[(u32)hole6_id - 1] : 0;
    if (hole6_c) {
        hole6_c->state = TCP_STATE_ESTABLISHED;
        hole6_c->send_unacknowledged = 1000;
        hole6_c->send_next = 1000;
        hole6_c->receive_next = 2000;
    }
    valid = valid && hole6_c &&
        !tcp_queue_send(&hole, hole6_id, hole_payload,
                        TCP_RETRANSMIT_DATA_MAX) &&
        tcp_prepare_transmit(&hole, hole6_id, 10, &hole_tx) == 1;
    hole_deadline = retransmission_deadline(hole6_c);
    valid = valid && hole_deadline &&
        tcp_tick(&hole, hole_deadline, &hole_tx) == 1 &&
        hole6_c->remote_mss == TCP_RETRANSMIT_DATA_MAX;
    hole_deadline = retransmission_deadline(hole6_c);
    valid = valid && hole_deadline &&
        tcp_tick(&hole, hole_deadline, &hole_tx) == 1 &&
        hole_tx.length == TCP_PMTU_IPV6_MIN - TCP_PMTU_HEADER6 &&
        hole6_c->remote_mss == TCP_PMTU_IPV6_MIN - TCP_PMTU_HEADER6 &&
        hole.stats.pmtu_blackholes == 3 &&
        hole_family == 6 && hole_mtu == TCP_PMTU_IPV6_MIN &&
        tcp_tick(&hole, retransmission_deadline(hole6_c),
                 &hole_tx) == 1 &&
        hole6_c->remote_mss == TCP_PMTU_IPV6_MIN - TCP_PMTU_HEADER6 &&
        hole.stats.pmtu_blackholes == 3;
    {
        static u8 bigpay[1460];
        static u8 built[TCP_HEADER_MIN + 1460];
        for (u32 index = 0; index < sizeof(bigpay); index++)
            bigpay[index] = (u8)(index * 3);
        length = tcp_build_ipv4(
            built, sizeof(built), client_address, server_address,
            1, 2, 10, 20, TCP_FLAG_ACK | TCP_FLAG_PSH, 65535,
            bigpay, sizeof(bigpay));
        valid = valid && length == TCP_HEADER_MIN + (int)sizeof(bigpay) &&
            !tcp_checksum_ipv4(client_address, server_address,
                               built, (u32)length);
        length = tcp_build_ipv6(
            built, sizeof(built), source6, destination6,
            1, 2, 10, 20, TCP_FLAG_ACK | TCP_FLAG_PSH, 65535,
            bigpay, sizeof(bigpay));
        valid = valid && length == TCP_HEADER_MIN + (int)sizeof(bigpay) &&
            !tcp_checksum_ipv6(source6, destination6, built, (u32)length);
        static u8 odd[17];
        static u8 oddseg[TCP_HEADER_MIN + 17];
        for (u32 index = 0; index < sizeof(odd); index++)
            odd[index] = (u8)(index + 1);
        length = tcp_build_ipv4(
            oddseg, sizeof(oddseg), client_address, server_address,
            1, 2, 9, 8, TCP_FLAG_ACK | TCP_FLAG_PSH, 65535,
            odd, sizeof(odd));
        valid = valid && length == TCP_HEADER_MIN + (int)sizeof(odd) &&
            !tcp_checksum_ipv4(client_address, server_address,
                               oddseg, (u32)length);
    }
    return valid ? 0 : -1;
}

static int tcp_pages_pump(struct tcp_context *client, u64 id,
                          struct tcp_context *server,
                          u32 client_address, u32 server_address) {
    u8 segment[TCP_HEADER_MIN + TCP_OPTION_MAX + TCP_RETRANSMIT_DATA_MAX];
    struct tcp_response reply;
    struct tcp_transmit transmit;
    for (u32 guard = 0; guard < 128; guard++) {
        struct tcp_connection *c = &client->connections[(u32)id - 1];
        if (!(c->send_loan_count ||
              c->send_buffer_offset < c->send_buffer_length))
            return 1;
        if (tcp_prepare_transmit(client, id, 100, &transmit) != 1 ||
            !transmit.length ||
            transmit.length > TCP_RETRANSMIT_DATA_MAX)
            return 0;
        int length = tcp_build_ipv4(
            segment, sizeof(segment), client_address, server_address,
            transmit.source_port, transmit.destination_port,
            transmit.sequence, transmit.acknowledgement,
            transmit.flags, transmit.window, transmit.data,
            transmit.length);
        if (length <= 0 ||
            tcp_receive_ipv4(server, client_address, server_address,
                             segment, (u32)length, &reply) ||
            !reply.valid || reply.flags != TCP_FLAG_ACK)
            return 0;
        length = tcp_build_ipv4(
            segment, sizeof(segment), server_address, client_address,
            reply.source_port, reply.destination_port,
            reply.sequence, reply.acknowledgement,
            reply.flags, reply.window, 0, 0);
        if (length <= 0 ||
            tcp_receive_ipv4(client, server_address, client_address,
                             segment, (u32)length, &reply))
            return 0;
    }
    return 0;
}

int test_tcp_pages64(void) {
    static struct tcp_context client;
    static struct tcp_context server;
    static u8 received[TCP_SEND_BUFFER_MAX];
    const u32 client_address = 0x0A00000Au;
    const u32 server_address = 0x0A00000Bu;
    u32 objects = object_active_count();
    u32 free_pages = pmm_free_pages();
    tcp_init(&client, 2000);
    tcp_init(&server, 2100);
    struct kernel_object *pages = page_resource_create();
    struct kernel_object *big = page_resource_create();
    int valid = pages && big && !page_resource_grow(pages, 2) &&
        !page_resource_grow(big, 64);
    struct page_resource *resource = pages ? page_resource_get(pages) : 0;
    if (valid && resource)
        for (u32 page = 0; page < resource->pages; page++) {
            u8 *bytes = (u8 *)(uptr_t)resource->physical[page];
            for (u32 index = 0; index < 4096; index++)
                bytes[index] = (u8)((page * 4096 + index) * 11 + 5);
        }
    valid = valid && resource && resource->pages == 2;
    u64 listener = tcp_listen(&server, server_address, 8090);
    u64 active = tcp_active_open(&client, client_address, 50010,
                                 server_address, 8090);
    valid = valid && listener && active;
    u32 client_isn = active
        ? client.connections[(u32)active - 1].send_unacknowledged : 0;
    u8 segment[TCP_HEADER_MIN + TCP_OPTION_MAX + TCP_RETRANSMIT_DATA_MAX];
    struct tcp_response reply;
    int length = tcp_build_ipv4(
        segment, sizeof(segment), client_address, server_address,
        50010, 8090, client_isn, 0, TCP_FLAG_SYN, 65535, 0, 0);
    valid = valid && length == TCP_HEADER_MIN &&
        !tcp_receive_ipv4(&server, client_address, server_address,
                          segment, (u32)length, &reply) &&
        reply.valid && reply.flags == (TCP_FLAG_SYN | TCP_FLAG_ACK);
    length = tcp_build_ipv4(
        segment, sizeof(segment), server_address, client_address,
        reply.source_port, reply.destination_port,
        reply.sequence, reply.acknowledgement,
        reply.flags, reply.window, 0, 0);
    valid = valid &&
        !tcp_receive_ipv4(&client, server_address, client_address,
                          segment, (u32)length, &reply) &&
        reply.valid && reply.flags == TCP_FLAG_ACK;
    length = tcp_build_ipv4(
        segment, sizeof(segment), client_address, server_address,
        reply.source_port, reply.destination_port,
        reply.sequence, reply.acknowledgement,
        reply.flags, reply.window, 0, 0);
    valid = valid && !tcp_receive_ipv4(
        &server, client_address, server_address,
        segment, (u32)length, &reply) && !reply.valid;
    u64 accepted_id = 0;
    for (u32 index = 0; index < TCP_CONNECTION_MAX; index++)
        if (server.connections[index].active &&
            server.connections[index].state == TCP_STATE_ESTABLISHED)
            accepted_id = ((u64)server.connections[index].generation << 32) |
                (index + 1);
    valid = valid && accepted_id;
    valid = valid &&
        !tcp_queue_send_pages(&client, active, pages, 3000, 3000) &&
        tcp_queue_send(&client, active, segment, 4) < 0 &&
        !tcp_queue_send_pages(&client, active, pages, 100, 200) &&
        tcp_pages_pump(&client, active, &server, client_address,
                       server_address) &&
        client.connections[(u32)active - 1].send_loan_count == 0;
    u32 received_length = 0;
    valid = valid && !tcp_receive_data(
        &server, accepted_id, received, sizeof(received),
        &received_length) && received_length == 3200;
    for (u32 index = 0; index < received_length; index++) {
        u32 source = index < 3000 ? 3000 + index : 100 + (index - 3000);
        if (received[index] != (u8)(source * 11 + 5)) valid = 0;
    }
    static const u8 legacy[4] = {0xC0, 0xFF, 0xEE, 0x42};
    valid = valid && !tcp_queue_send(&client, active, legacy, 4) &&
        tcp_queue_send_pages(&client, active, pages, 0, 16) < 0 &&
        tcp_pages_pump(&client, active, &server, client_address,
                       server_address) &&
        !tcp_receive_data(&server, accepted_id, received,
                          sizeof(received), &received_length) &&
        received_length == 4;
    for (u32 index = 0; index < received_length; index++)
        if (received[index] != legacy[index]) valid = 0;
    for (u32 index = 0; index < TCP_SEND_LOAN_MAX; index++)
        valid = valid && !tcp_queue_send_pages(
            &client, active, pages, 4000 + index, 1);
    valid = valid &&
        tcp_queue_send_pages(&client, active, pages, 4100, 1) < 0 &&
        tcp_pages_pump(&client, active, &server, client_address,
                       server_address) &&
        !tcp_receive_data(&server, accepted_id, received,
                          sizeof(received), &received_length) &&
        received_length == TCP_SEND_LOAN_MAX;
    for (u32 index = 0; index < received_length; index++)
        if (received[index] != (u8)((4000 + index) * 11 + 5)) valid = 0;
    valid = valid &&
        !tcp_queue_send_pages(&client, active, pages, 500, 100) &&
        !tcp_abort(&client, active, -19) &&
        client.connections[(u32)active - 1].send_loan_count == 0 &&
        !tcp_close(&client, active);
    u64 listener2 = tcp_listen(&server, server_address, 8091);
    u64 active2 = tcp_active_open(&client, client_address, 50011,
                                  server_address, 8091);
    valid = valid && listener2 && active2;
    u32 second_isn = active2
        ? client.connections[(u32)active2 - 1].send_unacknowledged : 0;
    length = tcp_build_ipv4(
        segment, sizeof(segment), client_address, server_address,
        50011, 8091, second_isn, 0, TCP_FLAG_SYN, 65535, 0, 0);
    valid = valid && length == TCP_HEADER_MIN &&
        !tcp_receive_ipv4(&server, client_address, server_address,
                          segment, (u32)length, &reply) &&
        reply.valid && reply.flags == (TCP_FLAG_SYN | TCP_FLAG_ACK);
    length = tcp_build_ipv4(
        segment, sizeof(segment), server_address, client_address,
        reply.source_port, reply.destination_port,
        reply.sequence, reply.acknowledgement,
        reply.flags, reply.window, 0, 0);
    valid = valid &&
        !tcp_receive_ipv4(&client, server_address, client_address,
                          segment, (u32)length, &reply) &&
        reply.valid && reply.flags == TCP_FLAG_ACK;
    length = tcp_build_ipv4(
        segment, sizeof(segment), client_address, server_address,
        reply.source_port, reply.destination_port,
        reply.sequence, reply.acknowledgement,
        reply.flags, reply.window, 0, 0);
    valid = valid && !tcp_receive_ipv4(
        &server, client_address, server_address,
        segment, (u32)length, &reply) && !reply.valid;
    for (u32 loan = 0; loan < 4; loan++)
        valid = valid && !tcp_queue_send_pages(
            &client, active2, big, 0, 64 * 4096);
    valid = valid &&
        tcp_queue_send_pages(&client, active2, big, 0, 4096) < 0 &&
        !tcp_close(&client, active2) &&
        client.connections[(u32)active2 - 1].send_loan_count == 0;
    if (pages) object_release(pages);
    if (big) object_release(big);
    valid = valid && object_active_count() == objects &&
        pmm_free_pages() == free_pages;
    return valid ? 0 : -1;
}
