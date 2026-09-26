# Net-bench results

First reference capture on real KVM hardware. Raw per-run rows live in
`net-bench-kvm-i3-7100u.csv`; regenerate the derived columns from any serial log
with `scripts/net-bench/collect.py --tsc-hz <invariant-tsc-hz>`.

## Hardware

- Host: Intel Core i3-7100U (Kaby Lake, 2 cores / 4 threads, 2.40 GHz base),
  invariant TSC, VT-x, AES-NI. Arch Linux 7.2.4, QEMU 11.1.1, edk2 OVMF.
- Guest: mich netbench profile, q35, `-enable-kvm -cpu host`, 256 MiB, virtio-net
  over slirp with a guestfwd to the host peer.
- TSC frequency used for the wall-time columns: 2.40 GHz (invariant, so cycles
  divide straight to seconds regardless of turbo).

## Loopback UDP round-trip (clean-stack upper bound)

The in-guest loopback path never touches the NIC, so it is the fairest measure of
the stack's own per-operation cost. Cycles are the primary figure; microseconds
are derived at 2.40 GHz. Median of three runs.

| Payload | min (cyc) | p50 (cyc) | p99 (cyc) | p50 latency |
| ------- | --------- | --------- | --------- | ----------- |
| 64 B    | ~68,700   | ~72,540   | ~82,400   | ~30.2 us    |
| 512 B   | ~69,060   | ~73,470   | ~92,500   | ~30.6 us    |
| 1400 B  | ~69,680   | ~74,620   | ~83,900   | ~31.0 us    |

Reading:

- The round-trip cost is flat across payload size (30.2 -> 31.0 us). The loopback
  RTT is dominated by fixed per-call overhead (two socket traversals plus the
  scheduler hop), not by the payload copy, which is what a clean small-message
  path should look like.
- ~30 us per round trip is ~33,000 blocking request/response exchanges per second
  single-threaded, before any NIC or hypervisor cost.
- These are the numbers to trust. Under TCG the same test read ~167k-189k
  "cycles" because TCG's rdtsc is not cycle-accurate; that is why the suite needs
  KVM for real figures. KVM is ~2.3-2.6x lower here, and it is the true cost.

## Wire path: a fixed stall, not a bandwidth wall

The guest-to-host wire tests over virtio-net + slirp are gated by a fixed,
payload-independent stall of roughly 0.42 s. This is the headline finding and it
is a protocol-timer effect, not throughput.

| Test              | measured             | wall time | note                    |
| ----------------- | -------------------- | --------- | ----------------------- |
| wire-tx 16 KiB    | ~1.0046e9 cyc        | ~0.419 s  | 0.31 Mbit/s (stalled)   |
| wire-rr 64 B p50  | ~1.076e9 cyc         | ~0.448 s  | per round trip          |
| wire-rr 64 B min  | ~0.904e9 cyc         | ~0.376 s  | best case still ~0.4 s  |

The tells: wire-tx cycle counts land within 0.02% of each other across three
runs (1004673304 / 1004576912 / 1004683846), and a 16 KiB transfer taking as
long as a single 64 B round trip only makes sense if both are waiting on the same
one-shot timer rather than moving bytes.

### What paces the wire path

An earlier draft blamed the TCP ACK-filter (delayed ACK). That was wrong: the
ACK-filter is off by default (`tcp->ack_filter = 0`; only a unit test enables it),
so it never runs in the bench. The real structural facts, read straight from the
tree:

- The APIC timer runs at 100 Hz (`apic64_timer_start(100)`), so one tick is 10 ms
  and every TCP timer is quantised to 10 ms.
- `TCP_RTO_INITIAL = 100` ticks = 1 second: the initial retransmit timeout is a
  whole second.
- The TCP timer engine (`net_interface_tick` -> `tcp_tick`: retransmit, RTO,
  persist, TIME_WAIT) is pumped from the virtio-net driver only when its IPv6
  maintenance timer fires, and that timer is periodic at 100 ticks, i.e. once per
  second. Bulk data still moves on NIC interrupts, but anything that falls back on
  a TCP timer waits on this coarse cadence.

So the fixed ~0.42 s wire stall is a guest-side timer-granularity and pump-cadence
effect, largely independent of QEMU. slirp packet loss can trigger a
timer-serviced retransmit, but the wait itself is the guest's own coarse cadence,
not slirp bandwidth. Correction to a second earlier assumption: the slow wire-rr
was blamed on TCG; KVM shows ~0.45 s per round trip too, so it is real guest
behaviour, not an emulation artifact.

### Localising it by measurement

`wire-tx` now reports the span split at the last byte handed to the stack:
`send=` cycles (guest TX pacing) versus `wait=` cycles (peer reply arriving across
the tick cadence). If `wait` dominates, the stall is on the receive/timer side; if
`send` dominates, it is TX pacing. Pair this with a host-side `perf kvm stat` VM-
exit count to separate the guest's coarse timer from any QEMU emulation cost.

### The fix: service the TCP timer after each RX, floor at the system tick

The driver now advances the TCP timer engine right after it drains a receive
batch, so an ACK that just arrived opens the window and the next segments ship in
the same loop iteration instead of waiting for the periodic maintenance timer.
That event-driven pump carries active connections, but a timer that fires with no
incoming packet (an RTO retransmit on an idle link) has nothing to trigger it, so
the periodic maintenance tick is kept as the backstop and tightened from 1 s to
10 ms (1 system tick), which is the cadence a networked kernel should service its
stack at anyway. An earlier revision proved the cause behind a `MICH_EAGER_TICK`
build flag that just raised the periodic rate; that knob is now folded into the
default and removed.

The wire-tx path in this bench hits exactly such a no-traffic timer wait, so its
floor is the maintenance period: the A/B below is against that period. Active
request/response traffic, where every exchange is an event, benefits from the
post-RX pump on top.

A/B, same emulator and disk. TCG measures the ratio (its cycle counts are not
wall time); KVM measures the wall time on the i3-7100U:

| build                   | wire-tx send | wire-tx wait   | wait wall (KVM) |
| ----------------------- | ------------ | -------------- | --------------- |
| before (1 Hz pump)      | ~733,000     | ~1,010,962,496 | ~419,000 us     |
| after (event + 10 ms)   | ~510,000     | ~24,312,478    | ~10,126 us      |

TCG numbers are from the final gated build. The wait phase drops ~42x under TCG
and ~41x on KVM (0.42 s -> 10 ms), while the send phase is unchanged. On KVM the
16 KiB gate throughput rises from 0.31 to 12.56 Mbit/s. Loopback RTT (~30 us) is
unaffected, as expected: it never waited on the pump. The KVM wall column is the
earlier 100 Hz-pump run; the final gated build should land at the same 10 ms floor
(re-confirm on hardware).

Note the post-RX pump is gated on packets actually drained, not run every loop
iteration: an ungated tick cost a maintenance syscall on every idle spin and
measured ~40M wait under TCG; gating it back to real RX events restored the ~24M
floor.

### Reference points (external, for orientation only)

Do not read these as a like-for-like ranking. mich here runs over QEMU slirp
usermode networking (not vhost/tap), single-threaded, without TSO/GSO or
zero-copy, so its absolute wire throughput is not comparable to a tuned Linux or
FreeBSD guest. They are here to show the order of magnitude and where the real
headroom is.

- Linux guest, virtio over KVM, single-stream: iperf3 ~13 Gbit/s, netperf
  TCP_STREAM ~12,612 Mbit/s. FreeBSD on the same setup: iperf3 ~6.5 Gbit/s,
  netperf single-stream ~1,233 Mbit/s.
- KVM guest-to-host virtio throughput ~1.8-2.1 Gbit/s with ~0.85 ms ping latency
  in older reports; virtio trades a little latency for throughput versus e1000.
- netperf TCP_RR round-trip latency on close cloud hosts ~66 us; KVM is reported
  to add roughly +90% latency over native.

The honest read: mich's clean-stack loopback RTT (~30 us) is already in the same
order as a real TCP_RR round trip, so the per-operation cost of the stack is
sound. The wire gap is transport/plumbing (slirp, no offloads, single queue), not
the protocol logic, and the tick fix removed the one bug that made the wire look
1000x worse than it is.

Bulk throughput and PPS numbers over tap/vhost are the next measurement, now that
the wire RTT is no longer pinned to the pump cadence.
