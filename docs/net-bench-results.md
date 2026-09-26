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

### Root cause: the TCP ACK-filter (delayed ACK)

`src/net/tcp.c` implements a FreeBSD-style ACK-filter (`data_ack`,
`TCP_TIMER_ACK_FILTER`): after acknowledging in-order data it suppresses pure
ACKs for one interval, floored at `TCP_ACK_FILTER_INTERVAL_MIN = TCP_RTO_INITIAL
= 100` ms, and a deadline-heap event flushes the held ACK later. For bulk streams
that is correct and saves ACKs. For a request/response ping-pong it is the
classic delayed-ACK penalty: each leg can wait on the held ACK plus however long
until the next `net_interface_tick` flushes it, which stacks up to the ~0.4 s we
measure per exchange.

Important correction to an earlier assumption: the slow wire-rr was previously
chalked up to TCG being slow. KVM shows ~0.45 s per round trip too, so this is
real stack behaviour, not an emulation artifact.

### Options (for discussion, not yet coded)

1. Disable the ACK-filter on the bench connection via the existing
   `tcp_set_ack_filter(tcp, 0)` and re-measure. Cleanest way to confirm the
   diagnosis and to get a real RR/latency number for the wire path.
2. Quick-ACK small request/response exchanges (send an immediate ACK when the
   segment carried a PSH and the receive buffer drained), keeping the filter for
   bulk. Closer to Linux `TCP_QUICKACK` / `tcp_in_quickack_mode`.
3. Check the `net_interface_tick` cadence during the bench; if ticks are coarse,
   the held ACK waits far past the 100 ms floor and inflates the stall further.

Bulk throughput and PPS numbers are deferred until the wire RTT is unstalled,
since a 0.4 s handshake tax swamps everything downstream.
