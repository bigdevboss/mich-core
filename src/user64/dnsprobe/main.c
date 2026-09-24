#include <mich/syscall.h>
#include <mich/dns.h>
#include <mich/timer.h>

// Exercises the resolver transport against a host-side responder reached
// through the QEMU guest forward. The parser is covered by dns_test.c in the
// kernel image; what only a real exchange can prove is the socket path:
// retries, the timeout, the switch to TCP, and the two-byte length prefix.
//
// The forward is TCP-only (QEMU rejects guestfwd=udp), so nothing ever answers
// the UDP attempts. That is deliberate: the resolver has to burn its retries,
// time out, and fall back before a single byte of the reply can arrive.

#define DNSPROBE_SERVER 0x0A000204u
#define DNSPROBE_NAME "probe.mich"
#define DNSPROBE_EXPECTED 0xC000024Du
// The capsule finishes DHCP within the first handful of ticks, so this only
// covers that gap. Each attempt costs a full UDP retry budget plus a TCP
// exchange, so a large count would outlast the profile timeout.
#define DNSPROBE_READY_ATTEMPTS 4u
#define DNSPROBE_READY_DELAY_TICKS 25u

static void wait_ticks(unsigned int ticks) {
    unsigned int deadline = mich_ticks() + ticks;
    while (mich_ticks() < deadline) mich_yield();
}

int main(void) {
    if (mich_dns_init(DNSPROBE_SERVER, 0)) {
        mich_write("Mich dnsprobe: resolver init FAIL\n");
        return 1;
    }
    mich_write("Mich dnsprobe: resolver ready\n");

    // The capsule brings the link up and finishes DHCP on its own schedule, so
    // there is no route to the responder for the first few hundred ticks.
    struct dns_result result;
    int resolved = -1;
    for (unsigned int attempt = 0;
         attempt < DNSPROBE_READY_ATTEMPTS && resolved; attempt++) {
        resolved = mich_dns_resolve(DNSPROBE_NAME, DNS_TYPE_A, &result);
        if (resolved) wait_ticks(DNSPROBE_READY_DELAY_TICKS);
    }
    if (resolved) {
        mich_write("Mich dnsprobe: resolve FAIL\n");
        return 1;
    }
    if (!result.ipv4_count || result.ipv4[0] != DNSPROBE_EXPECTED) {
        mich_write("Mich dnsprobe: address mismatch FAIL\n");
        return 1;
    }
    // The responder is reachable over TCP only, because QEMU has no UDP guest
    // forward. An answer therefore proves the UDP attempt produced nothing and
    // the resolver fell back, parsed the two-byte length prefix and accepted
    // the reply.
    mich_write("Mich dnsprobe: TCP fallback and framing pass\n");

    // The second lookup must not touch the wire at all.
    struct dns_result cached;
    if (mich_dns_cache_lookup(DNSPROBE_NAME, DNS_TYPE_A, &cached) ||
        cached.ipv4_count != result.ipv4_count ||
        cached.ipv4[0] != DNSPROBE_EXPECTED) {
        mich_write("Mich dnsprobe: cache FAIL\n");
        return 1;
    }
    mich_write("Mich dnsprobe: cached answer pass\n");

    // A name the responder refuses has to fail instead of inventing an answer.
    if (!mich_dns_resolve("absent.mich", DNS_TYPE_A, &result)) {
        mich_write("Mich dnsprobe: refusal accepted FAIL\n");
        return 1;
    }
    mich_write("Mich dnsprobe: refused name rejected pass\n");
    mich_write("Mich dnsprobe: transport pass\n");
    return 0;
}
