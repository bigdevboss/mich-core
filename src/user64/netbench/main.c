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

// The dial is retried because the first attempts can land before the virtio-net
// capsule has finished DHCP, and a spin budget bounds every socket wait so a
// silently dead peer fails the run instead of hanging it. The budget is patient
// because a single wire operation under TCG through slirp can take seconds of
// wall time; the loop exits the moment the bytes arrive, so a high ceiling only
// buys patience and never slows the common case.
#define CONNECT_ATTEMPTS 40u
#define CONNECT_DELAY_TICKS 25u
#define IO_SPINS 200000u

static u64 sample_cycles[SAMPLE_COUNT];
static u64 wire_cycles[WIRE_RR_COUNT];
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

// Receives exactly length bytes into out. The peer protocol is lock-step, so it
// never sends ahead and a chunk never overruns the request. Yields between reads
// because the bytes are delivered by the virtio capsule on another task.
static int stream_recv_exact(unsigned int handle, u8 *out, u32 length) {
    u32 got = 0;
    for (unsigned int spin = 0; spin < IO_SPINS && got < length; spin++) {
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
        }
        if (state.eof && got < length) return -1;
        mich_yield();
    }
    return got == length ? 0 : -1;
}

// Reads the peer's one-line reply into out, NUL-terminated. The reply is the
// only thing the peer writes after a bulk run, so reading to the newline cannot
// swallow data-phase bytes.
static int stream_recv_line(unsigned int handle, char *out, u32 capacity) {
    u32 used = 0;
    for (unsigned int spin = 0; spin < IO_SPINS; spin++) {
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
        }
        if (state.eof) {
            out[used] = '\0';
            return used ? 0 : -1;
        }
        mich_yield();
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
static int wire_rr(u32 count, u32 size) {
    int handle = connect_peer();
    if (handle < 0) return -1;
    char command[32];
    u32 command_length = build_command(command, "RR", count, 1, size);
    if (stream_send_all((unsigned int)handle, (const u8 *)command,
                        command_length)) {
        mich_handle_close((unsigned int)handle);
        return -1;
    }
    for (u32 index = 0; index < size; index++)
        wire_payload[index] = (u8)index;
    u64 total = 0;
    for (u32 round = 0; round < count; round++) {
        u64 start = read_cycles();
        if (stream_send_all((unsigned int)handle, wire_payload, size) ||
            stream_recv_exact((unsigned int)handle, wire_incoming, size)) {
            mich_handle_close((unsigned int)handle);
            return -1;
        }
        wire_cycles[round] = read_cycles() - start;
        total += wire_cycles[round];
        mich_yield();
    }
    mich_handle_close((unsigned int)handle);
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
static int wire_tx(u32 total_bytes) {
    int handle = connect_peer();
    if (handle < 0) return -1;
    char command[32];
    u32 command_length = build_command(command, "TX", total_bytes, 0, 0);
    if (stream_send_all((unsigned int)handle, (const u8 *)command,
                        command_length)) {
        mich_handle_close((unsigned int)handle);
        return -1;
    }
    for (u32 index = 0; index < MICH_SOCKET_STREAM_PAYLOAD_MAX; index++)
        wire_payload[index] = (u8)index;
    u64 start = read_cycles();
    u32 sent = 0;
    while (sent < total_bytes) {
        u32 take = total_bytes - sent;
        if (take > MICH_SOCKET_STREAM_PAYLOAD_MAX)
            take = MICH_SOCKET_STREAM_PAYLOAD_MAX;
        if (stream_send_all((unsigned int)handle, wire_payload, take)) {
            mich_handle_close((unsigned int)handle);
            return -1;
        }
        sent += take;
    }
    char reply[32];
    int status = stream_recv_line((unsigned int)handle, reply, sizeof(reply));
    u64 elapsed = read_cycles() - start;
    mich_handle_close((unsigned int)handle);
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
    mich_write("\n");
    return 0;
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
    // Bulk TX is the wire correctness gate: it is one-directional, so it proves
    // the connect, the stream send path, and the byte-accurate reply in about a
    // second even under TCG, without the per-round polling stalls the round trip
    // pays through slirp.
    if (wire_tx(WIRE_TX_TOTAL) != 0) {
        mich_write("Mich netbench: wire FAIL\n");
        return 1;
    }
    mich_write("Mich netbench: wire report pass\n");
    // Round-trip latency is reported after the gate and best-effort: each round
    // through slirp under TCG costs seconds and is dominated by the guest polling
    // for the reply (rdtsc counts the poll, not the wire), so the emulator may
    // tear the guest down before it finishes. That is harmless here, and the same
    // code produces real numbers fast on KVM with vhost where a round trip is
    // cheap. The return value is intentionally ignored for that reason.
    (void)wire_rr(WIRE_RR_COUNT, 64u);
    // A netbench boot runs on a quiescent kernel: the kernel detects this probe
    // in the module set and skips the driver-live-recovery lab that a resident
    // task would otherwise perturb. With no recovery sequence to race, the probe
    // just returns and gets reaped, freeing its slot for the rest of the boot.
    return 0;
}
