# Network stack benchmark

This measures the mich-core network stack, not the host it runs on. Every number
is taken inside the guest with `rdtsc`; the host-side peer never times anything.
The goal is a fair comparison against Linux and FreeBSD run as guests under the
same QEMU/KVM/virtio configuration, so the honest headline metric is work per
cycle, not absolute Gbit/s.

## Why these choices

- **Cycles per packet and per byte are the primary metric.** Absolute throughput
  depends on the host silicon and on whether KVM or TCG is in use; cycles per
  unit of work does not, so it is the only figure that compares cleanly across
  operating systems and across machines.
- **Two topologies, on purpose.**
  - *In-guest loopback* isolates the stack itself. The guest is both endpoints
    over the built-in loopback interface, so no virtio, host bridge, or second
    scheduler sits in the path. This is the upper bound for the stack.
  - *Guest to host over tap plus vhost* adds exactly one realistic datapath and
    nothing more. It is the number that reflects real NIC-bound work.
  - Guest-to-guest over a host bridge is deliberately excluded for now: a packet
    would cross `Guest1 -> QEMU1 -> host bridge -> QEMU2 -> Guest2`, so the run
    would measure the host scheduler and the Linux bridge, not the mich stack.
- **KVM is required for any wire number.** Under TCG the numbers describe the
  emulator. Use `-enable-kvm -cpu host`. Loopback numbers are still meaningful
  under TCG as a relative baseline, but label them as such.
- **A custom peer, not iperf3.** iperf3 would drag in its control protocol,
  cookie handshake, and JSON, none of which the freestanding guest should carry.
  A small purpose-built peer keeps the guest simple and the protocol auditable.
  The trade-off is that cross-OS comparison must use this same harness on every
  guest, rather than comparing against iperf3's published figures.

## Metrics

| Metric | What it captures | How |
| --- | --- | --- |
| Bulk TCP/UDP throughput | bytes/sec at payload sizes 64, 512, 1460, 9000 | timed byte counts |
| Small-packet PPS | 64-byte packets/sec, RX and TX separately | timed datagram counts |
| RR latency | round-trip p50/p99 and transactions/sec | per-round rdtsc |
| Cycles/packet, cycles/byte | CPU work per unit, the cross-OS metric | rdtsc spans / count |

The NIC-layer cycles/packet figure already exists in-kernel
(`vnic_bench64_run`, RX/TX/ECHO with min/avg/p50/p99). The socket-path bench adds
the full IP/UDP/TCP/socket cost on top, so the two together show where cycles go.

## Run discipline

- Each point runs at least 30 to 60 seconds, repeated three or more times.
- The first run is a warmup and is discarded.
- Report the median and p99, never a single best case.
- Record `lscpu` (model, base frequency, cores), the QEMU version, and whether
  KVM was enabled, next to every table. rdtsc counts reference cycles; convert to
  time only with the measured invariant-TSC frequency.

## Host peer

`scripts/net-bench/peer.c` is a standalone libc program: `cc -O2 -o peer peer.c`.
The guest is always the active side and opens one TCP control connection per
test. Protocol v1, all fields decimal ASCII, one command line per connection:

| Command | Peer behaviour | Guest measures |
| --- | --- | --- |
| `TX <total>` | drains `<total>` bytes, replies `OK <received>` | TCP send throughput |
| `RX <total>` | sends `<total>` bytes, then closes | TCP receive throughput |
| `RR <count> <size>` | echoes `<size>` bytes `<count>` times | TCP round-trip latency |
| `UTX <port>` | binds UDP, `READY`, counts datagrams until quiet, replies `OK <packets> <bytes>` | UDP send PPS |
| `URX <port> <count> <size>` | binds UDP, `READY`, learns guest address from one priming datagram, blasts `<count>` datagrams back | UDP receive PPS |
| `URR <port> <count> <size>` | UDP echo | UDP round-trip latency |

The peer reports `READY` only after the UDP bind succeeds, so the guest never
races the bind and no startup sleep is needed.

## Topology setup (guest to host)

Filled in with the exact `qemu-system-x86_64` KVM + virtio + tap/vhost invocation
once the guest bench module lands. Loopback needs no host setup: the guest talks
to itself.

## In-guest module and CI

The guest bench module (`src/user64/netbench`) runs the full UDP socket path over
loopback and reports cycles per round-trip. The kernel spawns it through the same
probe hook as the dns and tls probes, but recognises the `netbench` module and
skips the driver-live-recovery lab for that boot. The lab deliberately crashes and
restarts a driver capsule against a tight tick deadline; a resident bench task
both perturbs that timing (a spurious recovery panic) and skews the cycle counts,
so the benchmark wants a quiescent kernel. The `netbench` smoke profile is
therefore gated on boot-essential plus net plus netbench markers only, since the
full init CI battery (which is wired behind that same lab) stays covered by the
`disk-test` and `dns` profiles.

## Status

- [x] Host peer, protocol v1, self-tested over loopback (TX/RX/RR/UDP).
- [x] Guest bench module (loopback socket path; peer wiring is next).
- [x] Makefile targets and QEMU bench profile (`make test64-netbench`).
- [ ] Guest-to-host socket path from the module to the peer.
- [ ] Reference numbers on real KVM hardware.
