#include <mich/syscall.h>
#include <mich/socket.h>
#include <mich/timer.h>
#include <mich/event.h>

// Measures the cost of the socket path, which the in-kernel netbench does not
// reach: test_net_bench times the VNIC layer alone, so it never pays for the
// route lookup, the UDP and IPv4 builders, the loopback hand-off, and the
// datagram queue. This module runs a real send-then-receive over the loopback
// interface and reports the per-packet cycle cost the way test_net_bench does,
// so the two numbers together show where cycles go between the NIC and the API.
//
// Loopback first on purpose: it is the upper bound for the stack with no virtio,
// no host bridge, and no second scheduler in the path. The wire path then dials
// the host peer over virtio-net so the two sets of numbers bracket the cost the
// real NIC, the host bridge, and the far-side stack add on top of loopback. The
// guest is always the active side of the peer protocol; timing stays in-guest.

// A round trip stays entirely in the kernel over loopback, so the receiver is
// ready the moment the send returns and mich_socket_wait does not spin. The
// count is kept small on purpose: this profile shares the CPU with the whole
// boot-time test suite under the emulator, and a heavier run would starve those
// tasks past the harness timeout. It is enough for a representative median; the
// long runs that need a tight p99 belong on real KVM hardware, not in CI.
#define SAMPLE_COUNT 32u
#define WARMUP_COUNT 8u
#define LOOPBACK_ADDRESS 0x7F000001u
#define SENDER_PORT 15000u
#define RECEIVER_PORT 15001u

// The QEMU bench profile launches the host peer on the host loopback and bridges
// it to this fixed slirp address, the same way the tls profile reaches its test
// server. The guest dials it as the active side of the peer protocol.
#define PEER_ADDRESS 0x0A000204u
#define PEER_PORT 4500u

// Deliberately tiny. In CI the wire path runs under TCG through slirp, where a
// single round trip crosses the emulator twice and the guest polls for the reply
// (rdtsc counts every poll, so the cycle figures here are not real latency).
// This is a plumbing correctness check; the long runs that produce real numbers
// belong on KVM with vhost and a blocking wait, driven from the host peer.
#define WIRE_RR_COUNT 8u
#define WIRE_TX_TOTAL 16384u

// UDP wire path. QEMU's user netdev has no udp guestfwd form, so the tcp control
// alias 10.0.2.4 cannot carry datagrams; the peer's UDP port is reached at the
// slirp gateway 10.0.2.2 instead, which forwards to the host where the peer
// binds it on demand. The guest socket must bind the wire interface address, not
// INADDR_ANY: context_for_address maps 0 to the loopback context, so a send to
// the gateway would then fail the route-to-context match in socket_send_to.
// slirp always leases 10.0.2.15 to the first guest, and DHCP has already settled
// by the time the tcp wire gate above has passed.
#define GUEST_ADDRESS 0x0A00020Fu
#define PEER_UDP_ADDRESS 0x0A000202u
#define PEER_UDP_PORT 15100u
#define GUEST_UDP_PORT 15200u
#define WIRE_UDP_COUNT 64u

// The dial is retried because the first attempts can land before the virtio-net
// capsule has finished DHCP, and a spin budget bounds every socket wait so a
// silently dead peer fails the run instead of hanging it. The budget is patient
// because a single wire operation under TCG through slirp can take seconds of
// wall time; the loop exits the moment the bytes arrive, so a high ceiling only
// buys patience and never slows the common case.
#define CONNECT_ATTEMPTS 40u
#define CONNECT_DELAY_TICKS 25u
#define IO_SPINS 200000u
// Blocking-wait budget for the UDP control reads. Unlike IO_SPINS these are real
// descheduling waits, not busy spins, so the probe sleeps through the busy boot
// instead of burning a poll budget before the reply lands. The bound only guards
// against a stream of spurious wakeups with no data; a genuinely silent peer
// blocks until the harness timeout, which is the same outcome as a spin here.
#define STREAM_WAITS 100000u

static u64 sample_cycles[SAMPLE_COUNT];
static u64 wire_cycles[WIRE_RR_COUNT];
// The datagram request carries a 1472-byte payload, too large to sit on the
// 8-page module stack next to everything else, so it lives here.
static struct mich_socket_send_request udp_send_request;
static u8 wire_payload[MICH_SOCKET_STREAM_PAYLOAD_MAX];
static u8 wire_incoming[MICH_SOCKET_STREAM_PAYLOAD_MAX];

static u64 read_cycles(void) {
    u32 low;
    u32 high;
    // lfence keeps earlier work from drifting past the counter read, so a
    // sample brackets only the send and receive it is meant to time.
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) :: "memory");
    return ((u64)high << 32) | low;
}

// The freestanding modules only have mich_write(const char *), so numbers are
// formatted here the same way the kernel's serial64_dec does it.
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

// Insertion sort. SAMPLE_COUNT is small and the data is nearly sorted after a
// warmup, so the quadratic worst case never bites and the code stays obvious.
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

// One send-then-receive over loopback. Returns the byte count delivered, or 0
// on any failure so the caller can fail closed rather than record a bogus
// timing.
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
        // Yield outside the timed span so the concurrent test tasks keep making
        // progress. A loopback round trip never blocks, so without this the
        // benchmark would monopolise the CPU and starve everything else under
        // the emulator; the yield is deliberately not inside the measurement.
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

// Spins the connect state until the stream is up, an error surfaces, or the
// budget runs out. Yields each turn because the virtio capsule that completes
// the handshake runs on another task.
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

// Dials the host peer, retrying while DHCP settles. Returns a connected stream
// handle or -1.
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
    while (sent < length) {
        struct mich_socket_stream_data chunk;
        u32 take = length - sent;
        if (take > MICH_SOCKET_STREAM_PAYLOAD_MAX)
            take = MICH_SOCKET_STREAM_PAYLOAD_MAX;
        chunk.length = take;
        chunk.reserved = 0;
        for (u32 index = 0; index < take; index++)
            chunk.data[index] = data[sent + index];
        if (mich_socket_stream_send(handle, &chunk)) return -1;
        sent += take;
    }
    return 0;
}

// Receives exactly length bytes into out, blocking on the socket event between
// reads. The peer protocol is lock-step, so it never sends ahead and a chunk
// never overruns the request. Blocking rather than spinning matters because a
// probe reaches the wire phase while the boot test suite still competes for the
// CPU; sleeping until virtio delivers the next chunk keeps a starved probe from
// exhausting a poll budget before the bytes land.
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

// Reads the peer's one-line reply into out, NUL-terminated, blocking on the
// socket event between reads. The reply is the only thing the peer writes after a
// bulk run, so reading to the newline cannot swallow data-phase bytes. Blocking
// is deliberate: a netbench probe reaches the wire phase while the boot test
// suite is still competing for the CPU, so a busy spin can exhaust its poll
// budget before the reply arrives; sleeping until there is TCP activity and
// waking to read it does not. Level-triggered readiness is re-checked each turn,
// so a wakeup that races the state check cannot be lost.
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
            // A chunk was drained; loop to check for more before blocking again.
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

// Appends value as decimal ASCII at pos and returns the new position. Used to
// build the peer command line, which is all decimal fields.
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

// Round-trip latency over the wire: the guest echoes size bytes off the peer
// count times and times each exchange. This is the same shape as the loopback
// measurement, but the path now includes virtio TX/RX, the host bridge, and the
// peer's own stack, so the delta against loopback is the wire overhead.
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

// Bulk send throughput: the guest streams total_bytes to the peer, which drains
// and replies with its byte tally. The reply is checked against total_bytes so a
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
        if (stream_send_all(handle, wire_payload, take))
            return -1;
        sent += take;
    }
    // Split the span at the last byte handed to the stack. The wire path shows a
    // fixed ~0.4s cost that does not scale with payload; reporting the send phase
    // apart from the wait phase localises it, because the two have different
    // suspects: send is guest TX pacing, wait is the peer reply arriving across
    // the coarse 10ms tick and once-per-second TCP timer pump.
    u64 send_done = read_cycles();
    char reply[32];
    int status = stream_recv_line(handle, reply, sizeof(reply));
    u64 end = read_cycles();
    u64 elapsed = end - start;
    u64 send_cycles = send_done - start;
    u64 wait_cycles = end - send_done;
    if (status || reply[0] != 'O' || reply[1] != 'K' || reply[2] != ' ')
        return -1;
    u64 acked = 0;
    for (u32 index = 3; reply[index]; index++) {
        if (reply[index] < '0' || reply[index] > '9') return -1;
        acked = acked * 10u + (u64)(reply[index] - '0');
    }
    if (acked != total_bytes) return -1;
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

// Binds a datagram socket to the wire interface so its sends route out virtio
// instead of loopback. Returns the handle or -1; the caller owns the close.
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

// Small-datagram TX over the wire: the guest blasts count datagrams of size to
// the peer, then reads back the peer's tally over the shared control stream. UDP
// gives no delivery guarantee, so the sent-versus-received gap is the small-packet
// loss this path exists to localise against the lossless loopback numbers.
// Returns 1 when the peer counted at least one datagram (a report line is
// printed), 0 when it answered a clean zero (the control stream is still in sync,
// so the caller may continue), and -1 when the control stream itself broke.
static int wire_udp_tx(unsigned int control, u32 count, u32 size) {
    char command[32];
    // handle_utx reads only the port from the command; the guest owns how many
    // datagrams it sends and how big they are.
    u32 command_length = build_command(command, "UTX", PEER_UDP_PORT, 0, 0);
    char ready[16];
    if (stream_send_all(control, (const u8 *)command, command_length) ||
        stream_recv_line(control, ready, sizeof(ready)) ||
        ready[0] != 'R')
        // READY gates the blast so the guest never sends before the peer's bind.
        // A missing READY means the control stream is unusable, so report broken.
        return -1;
    int udp = open_wire_udp();
    // The peer has already promised to reply after its quiescence window, so a
    // local bind failure leaves an unread reply on the stream: treat it as broken
    // rather than desync the shared connection for the tests that follow.
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
        // A full virtio TX ring rejects the send; that is local backpressure,
        // not loss, so yield and retry the same datagram a bounded number of
        // times before conceding it.
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
    // Zero delivered is a clean result, not a stream error: the peer answered a
    // well-formed "OK 0 0", so the control stream is still in sync and the caller
    // may keep using it. slirp cannot route guest-to-host datagrams to a
    // host-bound port, so this is the expected user-net outcome; return 0 to say
    // "nothing to report, carry on" versus 1 for "real numbers printed".
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
    // Payload sizes span a tiny datagram, a mid frame, and one close to the
    // loopback MTU, so the per-packet floor and the per-byte slope are both
    // visible without depending on fragmentation.
    static const u32 sizes[3] = {64u, 512u, 1400u};
    for (u32 index = 0; index < 3; index++)
        if (measure_size(sender, receiver, sizes[index]) != 0) {
            mich_write("Mich netbench: loopback FAIL\n");
            return 1;
        }
    mich_write("Mich netbench: loopback report pass\n");
    // The wire path adds what loopback cannot reach: the virtio TX and RX rings,
    // the host bridge, and a second stack (the peer) on the far side. The QEMU
    // bench profile launches that peer and bridges it to PEER_ADDRESS, so a dial
    // failure here means broken plumbing and must fail the run, not be skipped.
    // Every wire test drives one shared control connection rather than dialling
    // per test: slirp does not reliably carry a guest's second outbound
    // connection to a host-bound peer (the first completes, later ones report
    // connected but never deliver and wedge on the reply), so pipelining TX, UDP,
    // and the round trip over the single connection that works is what keeps them
    // all reachable. It also matches how iperf and netperf keep one persistent
    // control channel, and drops the per-test handshake out of the measurement.
    int control = connect_peer();
    if (control < 0) {
        mich_write("Mich netbench: wire FAIL\n");
        return 1;
    }
    // Bulk TX is the wire correctness gate: it is one-directional, so it proves
    // the connect, the stream send path, and the byte-accurate reply in about a
    // second even under TCG, without the per-round polling stalls the round trip
    // pays through slirp. The commands are lock-step, so it also leaves the shared
    // stream clean for the tests that follow.
    if (wire_tx((unsigned int)control, WIRE_TX_TOTAL) != 0) {
        mich_write("Mich netbench: wire FAIL\n");
        mich_handle_close((unsigned int)control);
        return 1;
    }
    mich_write("Mich netbench: wire report pass\n");
    // Round-trip latency runs first among the best-effort tests because, unlike
    // UDP under user-net, it produces real numbers even here, and each UDP probe
    // below spends a fixed peer-side quiescence window waiting for datagrams that
    // slirp never delivers; running RR first keeps that dead time from starving it
    // of the emulator's teardown budget. Each round through slirp under TCG is
    // dominated by the guest polling for the reply (rdtsc counts the poll, not the
    // wire), so this is a plumbing check here and produces real numbers fast on
    // KVM with vhost. A non-zero return means the shared stream desynced, so skip
    // the rest rather than feed a corrupted stream into the UDP probes.
    int stream_ok = wire_rr((unsigned int)control, WIRE_RR_COUNT, 64u) == 0;
    // UDP bulk TX exercises the datagram path the tcp gate never touches: the
    // socket bind to the wire address, the UDP and IPv4 builders, and delivery to
    // the host peer. It is surfaced, not gated: the control handshake rides the
    // shared connection, but the datagrams still have to cross to the peer, and
    // slirp cannot route guest-to-host datagrams to a host-bound port, so under
    // the user-net CI profile the peer counts zero and no report line prints. It
    // produces real numbers on the tap/vhost (KVM) topology. Two sizes bracket the
    // per-packet floor (64B) and the near-MTU cost (1400B). A negative return
    // means the shared stream desynced, so stop before it corrupts a later test;
    // reconnecting is not an option because that is the second-connection path
    // slirp will not carry, which is why everything shares this one connection.
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
    // A netbench boot runs on a quiescent kernel: the kernel detects this probe
    // in the module set and skips the driver-live-recovery lab that a resident
    // task would otherwise perturb. With no recovery sequence to race, the probe
    // just returns and gets reaped, freeing its slot for the rest of the boot.
    return 0;
}
