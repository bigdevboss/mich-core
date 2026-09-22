#include "tcp.h"

//
// Exact 64-by-32 division (the kernel has no libgcc; 64-bit / would
// otherwise emit a __udivdi3 reference that fails to link on i386).
//
static u32 div64_by_32(u64 n, u32 d) {
    u32 rem = (u32)(n >> 32);
    u32 res = 0;
    u32 bits = 32;
    if (rem >= d) {
        u32 shift = 31;
        do {
            u32 resbit = rem >= d ? 1 : 0;
            res |= resbit << shift;
            if (resbit) rem -= d << shift;
        } while (shift--);
        bits = 0;
    }
    while (bits--) {
        u32 resbit = 0;
        rem = (rem << 1) | (u32)((n >> bits) & 1);
        if (rem >= d) {
            rem -= d;
            resbit = 1;
        }
        res = (res << 1) | resbit;
    }
    return res;
}

//
// Pluggable congestion control. Two policies ship in-tree:
//
//  reno  - the legacy Mich behavior (slow start + congestion avoidance,
//          dupACK-3 fast retransmit) extracted verbatim, with the RFC 6582
//          fix applied: an RTO must NOT reduce ssthresh.
//
//  bbr   - BBR v1 (RFC 9439) in pure u32 fixed point (Q10 gains, Q16
//          bandwidth). The bottleneck model is estimated from the ACK clock:
//          btl_bw is a max filter of delivered-bytes/interval over a 10 s
//          window, min_rtt a min filter over the same window. Phases:
//          STARTUP (cwnd tracks 2.885*BDP), DRAIN (cwnd falls to BDP),
//          PROBE_BW (1.0/1.25 gain pattern), PROBE_RTT (4-cwnd floor for
//          200 ms once the min_rtt window goes stale).
//
//  Hook contract: on_rtt fires once per fresh RTT sample (a single ACK may
//  retire several segments and thus fire on_rtt several times); on_ack fires
//  once per cumulative ACK. BBR therefore records min_rtt in on_rtt and
//  advances its phase machine exactly once per ACK, gated by bbr_round.
//
//  Documented simplifications vs the reference implementation: the startup
//  exit is "target reached two rounds in a row" instead of the ACK-clock
//  rate test; the DRAIN cwnd cut is flight/2 per ACK; there is no max_cwnd
//  ratchet (the retransmit ring already bounds in-flight at
//  TCP_RETRANSMISSION_MAX * MSS).
//

static u32 bbr_bdp(struct tcp_connection *c) {
    u32 bdp;
    if (!c->bbr_bw || !c->bbr_min_rtt) return 0;
    bdp = (u32)(((u64)c->bbr_bw * c->bbr_min_rtt) >> 16);
    if (bdp < c->remote_mss * 4) bdp = c->remote_mss * 4;
    if (bdp > 65535) bdp = 65535;
    return bdp;
}

static void reno_init(struct tcp_connection *c) {
    (void)c;
}

static void reno_on_ack(struct tcp_connection *c, u32 acked_bytes,
                        u32 flight, u32 now) {
    (void)flight; (void)now;
    u32 increase = acked_bytes ? acked_bytes : c->remote_mss;
    if (c->congestion_window >= c->slow_start_threshold)
        increase = c->remote_mss * c->remote_mss / c->congestion_window;
    if (!increase) increase = 1;
    if (increase > 65535u - c->congestion_window)
        c->congestion_window = 65535;
    else
        c->congestion_window += increase;
}

static void reno_on_rtt(struct tcp_connection *c, u32 sample,
                        u32 flight, u32 now) {
    (void)c; (void)sample; (void)flight; (void)now;
}

static void reno_on_fast_retransmit(struct tcp_connection *c,
                                    u32 duplicate_acks) {
    (void)duplicate_acks;
    c->slow_start_threshold = c->congestion_window / 2;
    if (c->slow_start_threshold < c->remote_mss * 2)
        c->slow_start_threshold = c->remote_mss * 2;
    c->congestion_window = c->slow_start_threshold + c->remote_mss * 3;
}

static void reno_on_rto(struct tcp_connection *c, u32 now) {
    (void)now;
        // RFC 6582: an RTO is not a loss event for ssthresh.
    c->congestion_window = c->remote_mss;
}

static const struct tcp_cc_ops reno_ops = {
    "reno",
    reno_init,
    reno_on_ack,
    reno_on_rtt,
    reno_on_fast_retransmit,
    reno_on_rto,
};

const struct tcp_cc_ops *tcp_cc_reno(void) {
    return &reno_ops;
}

static const u32 bbr_probe_gains[8] = {
    1024, 1024, 1024, 1280, 1024, 1024, 1280, 1024
};

static void bbr_init(struct tcp_connection *c) {
    c->bbr_phase = TCP_BBR_STARTUP;
    c->bbr_min_rtt = 0;
    c->bbr_min_rtt_deadline = 0;
    c->bbr_bw = 0;
    c->bbr_bw_window_end = 0;
    c->bbr_delivered = 0;
    c->bbr_interval_start = 0;
    c->bbr_probe_count = 0;
    c->bbr_probe_rtt_start = 0;
    c->bbr_startup_rounds = 0;
    c->bbr_round = 0;
}

static void bbr_on_rtt(struct tcp_connection *c, u32 sample,
                       u32 flight, u32 now) {
    (void)flight;
    if (sample &&
        (!c->bbr_min_rtt || now >= c->bbr_min_rtt_deadline ||
         sample < c->bbr_min_rtt)) {
        c->bbr_min_rtt = sample;
        c->bbr_min_rtt_deadline = now + TCP_BBR_WINDOW;
    }
    c->bbr_round = 1;
}

static void bbr_on_ack(struct tcp_connection *c, u32 acked_bytes,
                       u32 flight, u32 now) {
    c->bbr_delivered += acked_bytes;
    if (c->bbr_phase == TCP_BBR_DRAIN) {
        u32 bdp = bbr_bdp(c);
        if (bdp && flight <= bdp) {
            c->bbr_phase = TCP_BBR_PROBE_BW;
            return;
        }
        if (flight >= c->congestion_window / 2 &&
            c->congestion_window > bdp) {
            u32 cut = flight / 2;
            c->congestion_window = c->congestion_window > cut ?
                c->congestion_window - cut : bdp;
            if (c->congestion_window < bdp) c->congestion_window = bdp;
        }
    }
    if (!c->bbr_round) return;
    c->bbr_round = 0;
    if (c->bbr_interval_start && now > c->bbr_interval_start &&
        c->bbr_delivered) {
        u32 elapsed = now - c->bbr_interval_start;
        u64 numerator = (u64)c->bbr_delivered * 65536;
        u32 rate = div64_by_32(numerator, elapsed);
        if (now >= c->bbr_bw_window_end || rate > c->bbr_bw) {
            c->bbr_bw = rate;
            c->bbr_bw_window_end = now + TCP_BBR_WINDOW;
        }
    }
    c->bbr_delivered = 0;
    c->bbr_interval_start = now;
    u32 bdp = bbr_bdp(c);
    switch (c->bbr_phase) {
    case TCP_BBR_STARTUP:
        if (!c->bbr_bw || !c->bbr_min_rtt) {
            c->congestion_window += c->congestion_window / 8 +
                c->remote_mss;
            break;
        }
        u32 target = (bdp * 2885) >> 10;
        if (target > 65535) target = 65535;
        if (target > c->congestion_window)
            c->congestion_window = target;
        c->bbr_startup_rounds++;
        if (c->bbr_startup_rounds >= 2 && target <= c->congestion_window)
            c->bbr_phase = TCP_BBR_DRAIN;
        break;
    case TCP_BBR_DRAIN:
        break;
    case TCP_BBR_PROBE_BW:
        if (now >= c->bbr_min_rtt_deadline) {
            c->bbr_phase = TCP_BBR_PROBE_RTT;
            c->bbr_probe_rtt_start = now;
            c->congestion_window = bdp ? bdp / 4 : 0;
            if (c->congestion_window < c->remote_mss * 4)
                c->congestion_window = c->remote_mss * 4;
        } else {
            u32 gain = bbr_probe_gains[c->bbr_probe_count & 7];
            u32 base = bdp ? bdp : c->remote_mss * 4;
            c->congestion_window = base * gain >> 10;
            if (c->congestion_window < c->remote_mss * 4)
                c->congestion_window = c->remote_mss * 4;
            c->bbr_probe_count++;
        }
        break;
    case TCP_BBR_PROBE_RTT:
        if (now >= c->bbr_probe_rtt_start + TCP_BBR_PROBE_RTT_DURATION)
            c->bbr_phase = TCP_BBR_PROBE_BW;
        break;
    default:
        break;
    }
}

static void bbr_on_fast_retransmit(struct tcp_connection *c,
                                   u32 duplicate_acks) {
    (void)c; (void)duplicate_acks;
        // BBR does not react to isolated loss; the model absorbs it.
}

static void bbr_on_rto(struct tcp_connection *c, u32 now) {
    u32 bdp = bbr_bdp(c);
    if (c->bbr_min_rtt && now < c->bbr_min_rtt_deadline) {
        c->congestion_window = bdp ? bdp / 2 : 0;
        if (c->congestion_window < c->remote_mss * 4)
            c->congestion_window = c->remote_mss * 4;
    } else {
        c->congestion_window = c->remote_mss;
    }
    if (c->bbr_phase == TCP_BBR_STARTUP || c->bbr_phase == TCP_BBR_DRAIN)
        c->bbr_phase = c->bbr_min_rtt ? TCP_BBR_PROBE_BW : TCP_BBR_STARTUP;
}

static const struct tcp_cc_ops bbr_ops = {
    "bbr",
    bbr_init,
    bbr_on_ack,
    bbr_on_rtt,
    bbr_on_fast_retransmit,
    bbr_on_rto,
};

const struct tcp_cc_ops *tcp_cc_bbr(void) {
    return &bbr_ops;
}
