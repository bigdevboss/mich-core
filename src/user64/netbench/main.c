#include <mich/syscall.h>
#include <mich/socket.h>
#include <mich/timer.h>

// Measures the cost of the socket path, which the in-kernel netbench does not
// reach: test_net_bench times the VNIC layer alone, so it never pays for the
// route lookup, the UDP and IPv4 builders, the loopback hand-off, and the
// datagram queue. This module runs a real send-then-receive over the loopback
// interface and reports the per-packet cycle cost the way test_net_bench does,
// so the two numbers together show where cycles go between the NIC and the API.
//
// Loopback first on purpose: it is the upper bound for the stack with no virtio,
// no host bridge, and no second scheduler in the path. The wire path to the
// host peer is a later milestone that reuses this same measurement code.

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

static u64 sample_cycles[SAMPLE_COUNT];

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
    // A netbench boot runs on a quiescent kernel: the kernel detects this probe
    // in the module set and skips the driver-live-recovery lab that a resident
    // task would otherwise perturb. With no recovery sequence to race, the probe
    // just returns and gets reaped, freeing its slot for the rest of the boot.
    return 0;
}
