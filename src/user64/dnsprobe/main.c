#include <mich/syscall.h>
#include <mich/dns.h>
#include <mich/timer.h>
#include <mich/event.h>

// Exercises the resolver transport against a host-side responder over the QEMU
// guest forward. The parser is covered by dns_test.c; only a real exchange proves
// the socket path: retries, timeout, switch to TCP, two-byte length prefix.
// The forward is TCP-only (QEMU rejects guestfwd=udp), so the UDP attempts go
// unanswered by design and force the retry/timeout/fallback path.

#define DNSPROBE_SERVER 0x0A000204u
#define DNSPROBE_NAME "probe.mich"
#define DNSPROBE_EXPECTED 0xC000024Du
// Each attempt costs a full UDP retry budget plus a TCP exchange, so a large
// count would outlast the profile timeout.
#define DNSPROBE_READY_ATTEMPTS 4u
#define DNSPROBE_READY_DELAY_TICKS 25u

// Block on a timer rather than a yield loop: a yield loop steals scheduler turns
// from the driver capsule's live-recovery probe, which budgets ticks per phase.
static void wait_ticks(unsigned int ticks) {
    int timer = mich_timer_create();
    if (timer <= 0) {
        unsigned int deadline = mich_ticks() + ticks;
        while (mich_ticks() < deadline) mich_yield();
        return;
    }
    if (!mich_timer_arm((unsigned int)timer, ticks, 0))
        mich_timer_wait((unsigned int)timer);
    mich_handle_close((unsigned int)timer);
}

int main(void) {
    if (mich_dns_init(DNSPROBE_SERVER, 0)) {
        mich_write("Mich dnsprobe: resolver init FAIL\n");
        return 1;
    }
    mich_write("Mich dnsprobe: resolver ready\n");

    // No route to the responder until the capsule finishes DHCP (first few hundred ticks).
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
    // Responder is TCP-only, so an answer proves the UDP attempt produced nothing
    // and the resolver fell back, parsed the length prefix and accepted the reply.
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
