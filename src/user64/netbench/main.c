#include <mich/syscall.h>
#include <mich/socket.h>
#include <mich/timer.h>
#include <mich/event.h>

// Times the socket path (route lookup, UDP/IPv4 builders, loopback hand-off,
// datagram queue) that the in-kernel test_net_bench skips by timing the VNIC
// layer alone; the two numbers together show where cycles go between NIC and API.
// Loopback is the no-virtio upper bound; the wire path then dials a host peer so
// the pair bracket the NIC/bridge/far-stack cost. The guest is always the active
// side and timing stays in-guest.

// Kept small: this profile shares the CPU with the whole boot test suite under
// the emulator, and a heavier run starves them past the harness timeout.
#define SAMPLE_COUNT 32u
#define WARMUP_COUNT 8u
#define LOOPBACK_ADDRESS 0x7F000001u
#define SENDER_PORT 15000u
#define RECEIVER_PORT 15001u

// Fixed slirp address the bench profile bridges the host peer to.
#define PEER_ADDRESS 0x0A000204u
#define PEER_PORT 4500u

// Tiny by default: under TCG+slirp a round trip is dominated by the guest polling
// for the reply (rdtsc counts the poll, not the wire), so the default is only a
// plumbing check. MICH_NETBENCH_TAP selects the KVM tap/vhost topology where the
// figures reflect the guest stack, so sizes grow to a real measurement. make does
// not track flag changes, so rebuild clean when switching topology.
#ifdef MICH_NETBENCH_TAP
#define WIRE_RR_COUNT 1000u
#define WIRE_TX_TOTAL 67108864u
#else
#define WIRE_RR_COUNT 8u
#define WIRE_TX_TOTAL 16384u
#endif

// UDP wire path. slirp has no udp guestfwd, so datagrams reach the peer via the
// gateway 10.0.2.2; on tap/vhost the peer is a plain host socket at PEER_ADDRESS.
// The guest must bind its interface address, not INADDR_ANY: address 0 maps to
// the loopback context and would fail the route-to-context match in socket_send_to.
#define GUEST_ADDRESS 0x0A00020Fu
#ifdef MICH_NETBENCH_TAP
#define PEER_UDP_ADDRESS PEER_ADDRESS
#define WIRE_UDP_COUNT 50000u
#else
#define PEER_UDP_ADDRESS 0x0A000202u
#define WIRE_UDP_COUNT 64u
#endif
#define PEER_UDP_PORT 15100u
#define GUEST_UDP_PORT 15200u

// Retried because the first dials can land before the virtio-net capsule finishes
// DHCP; the spin budget is patient (a wire op under TCG+slirp costs seconds) but
// bounded so a dead peer fails instead of hanging. The loop exits on first bytes.
#define CONNECT_ATTEMPTS 40u
#define CONNECT_DELAY_TICKS 25u
#define IO_SPINS 200000u
#define STREAM_WAITS 100000u

// Zero-progress window for the bulk sender, in 10ms ticks. Bounded by elapsed
// time with no accepted chunk, NOT by wakeup count: socket_tcp_notify fires on
// every inbound segment and maintenance tick, so counting wakeups drained the
// budget in a fraction of a second and failed a live connection at a random
// offset. Above TCP RTO backoff, below the peer's 30s receive timeout.
#define SEND_STALL_TICKS 1000u

static u64 sample_cycles[SAMPLE_COUNT];
static u64 wire_cycles[WIRE_RR_COUNT];
// Static: the 1472-byte payload is too large for the 8-page module stack.
static struct mich_socket_send_request udp_send_request;
static u8 wire_payload[MICH_SOCKET_STREAM_PAYLOAD_MAX];
static u8 wire_incoming[MICH_SOCKET_STREAM_PAYLOAD_MAX];

static u64 read_cycles(void) {
    u32 low;
    u32 high;
    // lfence stops earlier work drifting past the counter read.
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) :: "memory");
    return ((u64)high << 32) | low;
}

static void write_decimal(u64 value) {
    char digits[21];
    u32 count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    char text[22];
    u32 index = 0;
    while (count) text[index++] = digits[--count];
    text[index] = '\0';
    mich_write(text);
}

// Insertion sort: the sample count is small and the data nearly sorted.
static void sort_samples(u64 *values, u32 count) {
    for (u32 i = 1; i < count; i++) {
        u64 key = values[i];
        u32 j = i;
        while (j && values[j - 1] > key) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = key;
    }
}

static int bind_socket(int handle, u32 port) {
    struct mich_socket_bind_request request;
    request.address = LOOPBACK_ADDRESS;
    request.port = (u16)port;
    request.reserved = 0;
    return mich_socket_bind((unsigned int)handle, &request);
}

// Returns bytes delivered, or 0 on any failure so the caller fails closed.
static u32 round_trip(int sender, int receiver, u32 length) {
    static struct mich_socket_send_request send_request;
    static struct mich_socket_receive_result receive_result;
    send_request.destination_address = LOOPBACK_ADDRESS;
    send_request.destination_port = (u16)RECEIVER_PORT;
    send_request.length = (u16)length;
    if (mich_socket_send_to((unsigned int)sender, &send_request) != 0) return 0;
    if (mich_socket_wait((unsigned int)receiver) != 0) return 0;
    if (mich_socket_receive_from((unsigned int)receiver, &receive_result) != 0)
        return 0;
    return receive_result.length;
}

static int measure_size(int sender, int receiver, u32 length) {
    for (u32 i = 0; i < WARMUP_COUNT; i++) {
        if (round_trip(sender, receiver, length) != length) return -1;
        mich_yield();
    }
    u64 total = 0;
    for (u32 i = 0; i < SAMPLE_COUNT; i++) {
        u64 start = read_cycles();
        u32 delivered = round_trip(sender, receiver, length);
        u64 elapsed = read_cycles() - start;
        if (delivered != length) return -1;
        sample_cycles[i] = elapsed;
        total += elapsed;
        // Yield outside the timed span: a loopback round trip never blocks, so
        // without this the bench monopolises the CPU and starves the test tasks.
        mich_yield();
    }
    sort_samples(sample_cycles, SAMPLE_COUNT);
    mich_write("Mich netbench: loopback-udp size=");
    write_decimal(length);
    mich_write("B min=");
    write_decimal(sample_cycles[0]);
    mich_write(" avg=");
    write_decimal(total / SAMPLE_COUNT);
    mich_write(" p50=");
    write_decimal(sample_cycles[SAMPLE_COUNT / 2u]);
    mich_write(" p99=");
    write_decimal(sample_cycles[(SAMPLE_COUNT * 99u) / 100u]);
    mich_write(" cycles/round-trip\n");
    return 0;
}

static void sleep_ticks(unsigned int ticks) {
    int timer = mich_timer_create();
    if (timer <= 0) return;
    if (!mich_timer_arm((unsigned int)timer, ticks, 0))
        mich_timer_wait((unsigned int)timer);
    mich_handle_close((unsigned int)timer);
}

// Yields each turn: the virtio capsule that completes the handshake is another task.
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

// Dials the host peer, retrying while DHCP settles. Returns a stream handle or -1.
static int connect_peer(void) {
    for (unsigned int attempt = 0; attempt < CONNECT_ATTEMPTS; attempt++) {
        int handle = mich_socket_stream_create();
        if (handle <= 0) return -1;
        struct mich_socket_stream_connect_request connect;
        connect.interface_handle = 0;
        connect.destination_address = PEER_ADDRESS;
        connect.destination_port = (u16)PEER_PORT;
        connect.reserved = 0;
        if (!mich_socket_stream_connect((unsigned int)handle, &connect) &&
            !wait_connected((unsigned int)handle))
            return handle;
        mich_handle_close((unsigned int)handle);
        sleep_ticks(CONNECT_DELAY_TICKS);
    }
    return -1;
}

static int stream_send_all(unsigned int handle, const u8 *data, u32 length) {
    u32 sent = 0;
    unsigned int progress_tick = mich_ticks();
    unsigned int wakes = 0;
    while (sent < length) {
        struct mich_socket_stream_data chunk;
        u32 take = length - sent;
        if (take > MICH_SOCKET_STREAM_PAYLOAD_MAX)
            take = MICH_SOCKET_STREAM_PAYLOAD_MAX;
        chunk.length = take;
        chunk.reserved = 0;
        for (u32 index = 0; index < take; index++)
            chunk.data[index] = data[sent + index];
        if (!mich_socket_stream_send(handle, &chunk)) {
            sent += take;
            progress_tick = mich_ticks();
            wakes = 0;
            continue;
        }
        // A refused chunk is back-pressure (send buffer full), not a fault. Block
        // on the socket event rather than spin, so throughput follows the ACK
        // cadence; the event is remembered, so a racing ACK cannot be lost.
        struct mich_socket_stream_state_result state;
        state.readiness = 0;
        state.state = 0;
        state.error = 0;
        if (mich_socket_stream_state(handle, &state)) {
            mich_write("Mich netbench: send state query failed\n");
            return -1;
        }
        if (state.readiness & (SOCKET_READY_ERROR | SOCKET_READY_HANGUP)) {
            // Report state/error so a SEND FAIL separates a reset from a half-close.
            mich_write("Mich netbench: send broke state=");
            write_decimal(state.state);
            mich_write(" ready=");
            write_decimal(state.readiness);
            mich_write(" err=");
            if (state.error < 0) {
                mich_write("-");
                write_decimal((u64)(-(i64)state.error));
            } else {
                write_decimal((u64)state.error);
            }
            mich_write("\n");
            return -1;
        }
        // Unsigned tick subtraction wraps cleanly across a counter rollover. The
        // reported wake count separates a spurious-wakeup storm from a silent peer.
        if (mich_ticks() - progress_tick >= SEND_STALL_TICKS) {
            mich_write("Mich netbench: send stall wakes=");
            write_decimal(wakes);
            mich_write(" ready=");
            write_decimal(state.readiness);
            mich_write("\n");
            return -1;
        }
        wakes++;
        mich_socket_wait(handle);
    }
    return 0;
}

// Receives exactly length bytes, blocking on the socket event between reads (the
// probe competes with the boot test suite, so a busy spin would exhaust its budget
// before the bytes land). The lock-step peer never sends ahead, so no overrun.
static int stream_recv_exact(unsigned int handle, u8 *out, u32 length) {
    u32 got = 0;
    for (unsigned int wait = 0; wait < STREAM_WAITS && got < length; wait++) {
        struct mich_socket_stream_state_result state;
        state.readiness = 0;
        state.eof = 0;
        if (mich_socket_stream_state(handle, &state)) return -1;
        if (state.readiness & SOCKET_READY_ERROR) return -1;
        if (state.readiness & SOCKET_READY_READABLE) {
            struct mich_socket_stream_data chunk;
            chunk.length = 0;
            chunk.reserved = 0;
            if (mich_socket_stream_receive(handle, &chunk)) return -1;
            for (u32 index = 0; index < chunk.length && got < length; index++)
                out[got++] = chunk.data[index];
            continue;
        }
        if (state.eof && got < length) return -1;
        mich_socket_wait(handle);
    }
    return got == length ? 0 : -1;
}

// Reads the peer's one-line reply, NUL-terminated, blocking between reads. The
// reply is the only thing the peer writes after a bulk run, so reading to the
// newline cannot swallow data-phase bytes.
static int stream_recv_line(unsigned int handle, char *out, u32 capacity) {
    u32 used = 0;
    for (unsigned int wait = 0; wait < STREAM_WAITS; wait++) {
        struct mich_socket_stream_state_result state;
        state.readiness = 0;
        state.eof = 0;
        if (mich_socket_stream_state(handle, &state)) return -1;
        if (state.readiness & SOCKET_READY_ERROR) return -1;
        if (state.readiness & SOCKET_READY_READABLE) {
            struct mich_socket_stream_data chunk;
            chunk.length = 0;
            chunk.reserved = 0;
            if (mich_socket_stream_receive(handle, &chunk)) return -1;
            for (u32 index = 0; index < chunk.length; index++) {
                char c = (char)chunk.data[index];
                if (c == '\n') {
                    out[used] = '\0';
                    return 0;
                }
                if (used + 1 < capacity) out[used++] = c;
            }
            continue;
        }
        if (state.eof) {
            out[used] = '\0';
            return used ? 0 : -1;
        }
        mich_socket_wait(handle);
    }
    return -1;
}

static u32 append_u32(char *out, u32 pos, u64 value) {
    char digits[21];
    u32 count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (count) out[pos++] = digits[--count];
    return pos;
}

static u32 build_command(char *out, const char *verb, u64 a, int has_b, u64 b) {
    u32 pos = 0;
    for (u32 index = 0; verb[index]; index++) out[pos++] = verb[index];
    out[pos++] = ' ';
    pos = append_u32(out, pos, a);
    if (has_b) {
        out[pos++] = ' ';
        pos = append_u32(out, pos, b);
    }
    out[pos++] = '\n';
    return pos;
}

// Round-trip latency over the wire; same shape as the loopback measurement, so
// the delta against loopback is the virtio/bridge/far-stack overhead.
static int wire_rr(unsigned int handle, u32 count, u32 size) {
    char command[32];
    u32 command_length = build_command(command, "RR", count, 1, size);
    if (stream_send_all(handle, (const u8 *)command, command_length))
        return -1;
    for (u32 index = 0; index < size; index++)
        wire_payload[index] = (u8)index;
    u64 total = 0;
    for (u32 round = 0; round < count; round++) {
        u64 start = read_cycles();
        if (stream_send_all(handle, wire_payload, size) ||
            stream_recv_exact(handle, wire_incoming, size))
            return -1;
        wire_cycles[round] = read_cycles() - start;
        total += wire_cycles[round];
        mich_yield();
    }
    sort_samples(wire_cycles, count);
    mich_write("Mich netbench: wire-rr size=");
    write_decimal(size);
    mich_write("B min=");
    write_decimal(wire_cycles[0]);
    mich_write(" avg=");
    write_decimal(total / count);
    mich_write(" p50=");
    write_decimal(wire_cycles[count / 2u]);
    mich_write(" p99=");
    write_decimal(wire_cycles[(count * 99u) / 100u]);
    mich_write(" cycles/round-trip\n");
    return 0;
}

// Bulk send throughput: stream total_bytes; the reply tally is checked so a
// truncated stream fails loud instead of reporting a fast but wrong number.
static int wire_tx(unsigned int handle, u32 total_bytes) {
    char command[32];
    u32 command_length = build_command(command, "TX", total_bytes, 0, 0);
    if (stream_send_all(handle, (const u8 *)command, command_length))
        return -1;
    for (u32 index = 0; index < MICH_SOCKET_STREAM_PAYLOAD_MAX; index++)
        wire_payload[index] = (u8)index;
    u64 start = read_cycles();
    u32 sent = 0;
    while (sent < total_bytes) {
        u32 take = total_bytes - sent;
        if (take > MICH_SOCKET_STREAM_PAYLOAD_MAX)
            take = MICH_SOCKET_STREAM_PAYLOAD_MAX;
        if (stream_send_all(handle, wire_payload, take)) {
            // The byte offset localises a wedge: 0 is a connect fault, full length
            // a reply fault, partway a TX back-pressure stall.
            mich_write("Mich netbench: wire-tx SEND FAIL at=");
            write_decimal(sent);
            mich_write(" of=");
            write_decimal(total_bytes);
            mich_write("\n");
            return -1;
        }
        sent += take;
    }
    // Split send from wait: the wire shows a fixed cost that does not scale with
    // payload, and the two phases have different suspects (guest TX pacing vs the
    // peer reply arriving across the coarse tick).
    u64 send_done = read_cycles();
    char reply[32];
    int status = stream_recv_line(handle, reply, sizeof(reply));
    u64 end = read_cycles();
    u64 elapsed = end - start;
    u64 send_cycles = send_done - start;
    u64 wait_cycles = end - send_done;
    if (status || reply[0] != 'O' || reply[1] != 'K' || reply[2] != ' ') {
        // Payload sent but no valid tally: the wait phase is at fault, not send.
        mich_write("Mich netbench: wire-tx REPLY FAIL\n");
        return -1;
    }
    u64 acked = 0;
    for (u32 index = 3; reply[index]; index++) {
        if (reply[index] < '0' || reply[index] > '9') return -1;
        acked = acked * 10u + (u64)(reply[index] - '0');
    }
    if (acked != total_bytes) {
        // Fewer bytes counted than sent: the stream truncated in flight.
        mich_write("Mich netbench: wire-tx ACK MISMATCH acked=");
        write_decimal(acked);
        mich_write(" sent=");
        write_decimal(total_bytes);
        mich_write("\n");
        return -1;
    }
    mich_write("Mich netbench: wire-tx bytes=");
    write_decimal(total_bytes);
    mich_write(" cycles=");
    write_decimal(elapsed);
    mich_write(" send=");
    write_decimal(send_cycles);
    mich_write(" wait=");
    write_decimal(wait_cycles);
    mich_write("\n");
    return 0;
}

// Binds a datagram socket to the wire interface so sends route out virtio, not
// loopback. Returns the handle or -1; the caller owns the close.
static int open_wire_udp(void) {
    int udp = mich_socket_create();
    if (udp <= 0) return -1;
    struct mich_socket_bind_request bind;
    bind.address = GUEST_ADDRESS;
    bind.port = (u16)GUEST_UDP_PORT;
    bind.reserved = 0;
    if (mich_socket_bind((unsigned int)udp, &bind)) {
        mich_handle_close((unsigned int)udp);
        return -1;
    }
    return udp;
}

// Small-datagram TX over the wire, tally read back over the shared control stream.
// Returns 1 when the peer counted >=1 datagram (report printed), 0 on a clean zero
// (stream still in sync, caller may continue), -1 when the control stream broke.
static int wire_udp_tx(unsigned int control, u32 count, u32 size) {
    char command[32];
    u32 command_length = build_command(command, "UTX", PEER_UDP_PORT, 0, 0);
    char ready[16];
    if (stream_send_all(control, (const u8 *)command, command_length) ||
        stream_recv_line(control, ready, sizeof(ready)) ||
        ready[0] != 'R')
        // READY gates the blast until the peer's bind; missing it means broken stream.
        return -1;
    int udp = open_wire_udp();
    if (udp < 0) return -1;
    for (u32 index = 0; index < size; index++)
        udp_send_request.payload[index] = (u8)index;
    udp_send_request.destination_address = PEER_UDP_ADDRESS;
    udp_send_request.destination_port = (u16)PEER_UDP_PORT;
    udp_send_request.length = (u16)size;
    u32 sent = 0;
    u64 start = read_cycles();
    for (u32 round = 0; round < count; round++) {
        u32 attempt = 0;
        // A full virtio TX ring is local back-pressure, not loss: yield and retry
        // the same datagram a bounded number of times before conceding it.
        while (mich_socket_send_to((unsigned int)udp, &udp_send_request)) {
            if (++attempt >= 8u) break;
            mich_yield();
        }
        if (attempt < 8u) sent++;
        mich_yield();
    }
    u64 elapsed = read_cycles() - start;
    char reply[64];
    int status = stream_recv_line(control, reply, sizeof(reply));
    mich_handle_close((unsigned int)udp);
    if (status || reply[0] != 'O' || reply[1] != 'K' || reply[2] != ' ')
        return -1;
    u64 packets = 0;
    u32 index = 3;
    while (reply[index] >= '0' && reply[index] <= '9')
        packets = packets * 10u + (u64)(reply[index++] - '0');
    if (reply[index] != ' ') return -1;
    index++;
    u64 bytes = 0;
    while (reply[index] >= '0' && reply[index] <= '9')
        bytes = bytes * 10u + (u64)(reply[index++] - '0');
    // Zero delivered is clean, not an error: slirp cannot route guest-to-host
    // datagrams to a host-bound port, so "OK 0 0" is the expected user-net outcome.
    if (!packets) return 0;
    mich_write("Mich netbench: wire-udp-tx size=");
    write_decimal(size);
    mich_write("B sent=");
    write_decimal(sent);
    mich_write(" packets=");
    write_decimal(packets);
    mich_write(" bytes=");
    write_decimal(bytes);
    mich_write(" cycles=");
    write_decimal(elapsed);
    mich_write("\n");
    return 1;
}

int main(void) {
    int sender = mich_socket_create();
    int receiver = mich_socket_create();
    if (sender <= 0 || receiver <= 0) {
        mich_write("Mich netbench: socket FAIL\n");
        return 1;
    }
    if (bind_socket(sender, SENDER_PORT) != 0 ||
        bind_socket(receiver, RECEIVER_PORT) != 0) {
        mich_write("Mich netbench: bind FAIL\n");
        return 1;
    }
    // Sizes span a tiny datagram, a mid frame, and near-MTU, so the per-packet
    // floor and per-byte slope are both visible without fragmentation.
    static const u32 sizes[3] = {64u, 512u, 1400u};
    for (u32 index = 0; index < 3; index++)
        if (measure_size(sender, receiver, sizes[index]) != 0) {
            mich_write("Mich netbench: loopback FAIL\n");
            return 1;
        }
    mich_write("Mich netbench: loopback report pass\n");
    // All wire tests share ONE control connection: slirp does not reliably carry a
    // guest's second outbound connection to a host-bound peer (later dials report
    // connected but wedge on the reply), so pipelining over the one that works is
    // what keeps them reachable. A dial failure here is broken plumbing, not a skip.
    int control = connect_peer();
    if (control < 0) {
        mich_write("Mich netbench: wire FAIL\n");
        return 1;
    }
    // Bulk TX is the wire correctness gate: one-directional, so it proves connect,
    // stream send, and the byte-accurate reply fast even under TCG (no per-round
    // slirp poll stalls), and its lock-step commands leave the shared stream clean.
    if (wire_tx((unsigned int)control, WIRE_TX_TOTAL) != 0) {
        mich_write("Mich netbench: wire FAIL\n");
        mich_handle_close((unsigned int)control);
        return 1;
    }
    mich_write("Mich netbench: wire report pass\n");
    // RR runs before the UDP probes because it yields real numbers even under
    // user-net, while each UDP probe burns a fixed peer-side quiescence window;
    // ordering RR first keeps that dead time from starving its teardown budget.
    // A non-zero return means the shared stream desynced, so skip the rest.
    int stream_ok = wire_rr((unsigned int)control, WIRE_RR_COUNT, 64u) == 0;
    // UDP bulk TX exercises the datagram path the tcp gate never touches (wire bind,
    // UDP/IPv4 builders, delivery to the peer). Surfaced, not gated: under user-net
    // slirp drops guest-to-host datagrams, so the peer counts zero and no line
    // prints; real numbers come on tap/vhost. Two sizes bracket floor and near-MTU.
    // A negative return means desync, so stop (reconnecting is the second-connection
    // path slirp will not carry, hence the single shared connection).
    int udp_reported = 0;
    if (stream_ok) {
        int udp = wire_udp_tx((unsigned int)control, WIRE_UDP_COUNT, 64u);
        if (udp >= 0) {
            udp_reported |= udp;
            udp = wire_udp_tx((unsigned int)control, WIRE_UDP_COUNT, 1400u);
            udp_reported |= (udp > 0);
        }
    }
    if (udp_reported) mich_write("Mich netbench: udp report pass\n");
    mich_handle_close((unsigned int)control);
    // The kernel detects this probe in the module set and skips the driver-live-
    // recovery lab, so the probe just returns and gets reaped.
    return 0;
}
