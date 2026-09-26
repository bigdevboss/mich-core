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
    nothing more. It is the number that reflects real NIC-bound work. Guest to
    host over slirp is the zero-setup stand-in: it needs no host networking and,
    under KVM, already gives real cycles, so it is the first wire number to take
    before the tap/vhost plumbing is in place.
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
The guest is always the active side. It opens one TCP control connection and
pipelines every test over it: a sequence of command lines, each followed by its
own data phase, until the guest closes. This is deliberate. Under QEMU user-mode
networking (slirp) a guest's first outbound connection to a host-bound peer works
but later ones report connected and then never deliver, so a single reused
connection is what keeps all the tests reachable; it also matches how iperf and
netperf keep one persistent control channel. Each handler consumes exactly its
data phase (`TX` reads exactly `<total>` bytes, `RR` exactly `<count>*<size>`, the
UDP tests run on a side socket), so the stream is always left at the next command
line. The peer forks a child per connection so a silent client cannot wedge it.
Protocol, all fields decimal ASCII:

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

## Running on KVM hardware

The disk image is not committed, so build it first. This needs `gcc`, `nasm`, and
`python3`; running also needs `qemu-system-x86_64` and OVMF.

```
make bin/x86_64/disk-netbench.img
```

Every run below is driven through `scripts/qemu-smoke64.sh`, which already builds
and launches the peer, wires the guest to it, boots under OVMF, and checks the
pass markers. `MICH_KVM=1` swaps the emulator for `-enable-kvm -cpu host`, which
is what makes `rdtsc` count real host cycles instead of the emulator's.
`MICH_QEMU_TIMEOUT` shortens the wait, since a KVM boot finishes in seconds.

### Topology 1: in-guest loopback (clean-stack upper bound)

No networking, no peer: the guest is both endpoints over its loopback interface.

```
MICH_KVM=1 MICH_QEMU_TIMEOUT=90 make test64-netbench
```

The `loopback-udp` lines are the numbers for this topology.

### Topology 2: guest to host over slirp (works today, no network setup)

Same command; it is the wire pass of the same run. The script starts the peer on
`127.0.0.1:4500` and bridges the guest's `10.0.2.4:4500` to it. This is not
tap/vhost, but under KVM it already gives real host cycles, low datagram loss, and
fast round trips, so it is the recommended first wire number. The `wire-tx`,
`wire-rr`, and `wire-udp-tx` lines are this topology.

### Topology 3: guest to host over tap plus vhost (max throughput, advanced)

This removes slirp from the path but needs host-side plumbing, because the guest
runs real DHCP and the current addresses are the slirp subnet (`10.0.2.0/24`,
guest lease `10.0.2.15`, TCP control at `10.0.2.4`, UDP peer at `10.0.2.2`). On
the host, as root:

```
ip tuntap add dev tap0 mode tap
ip addr add 10.0.2.2/24 dev tap0
ip addr add 10.0.2.4/24 dev tap0        # TCP control alias the guest dials
ip link set tap0 up
dnsmasq --interface=tap0 --bind-interfaces --except-interface=lo \
        --dhcp-range=10.0.2.15,10.0.2.15,255.255.255.0,12h \
        --dhcp-option=3,10.0.2.2 --no-daemon &
cc -O2 -o peer scripts/net-bench/peer.c
./peer --port 4500 &
```

Then boot the guest by hand against the tap with a vhost-backed virtio-net,
pointing it at the same disk and OVMF the script uses (see the invocation it
prints, or `scripts/uefi-firmware.sh` for the OVMF paths). The netdev is:

```
-netdev tap,id=michnet,ifname=tap0,script=no,downscript=no,vhost=on \
-device virtio-net-pci,netdev=michnet
```

This path is not yet automated or verified here; if you want it wired into the
runner and the peer addresses made configurable, that is a small follow-up.

## What to send back

Paste the following so the numbers can be turned into a comparison table:

```
lscpu                              # model, base MHz, invariant TSC, flags
qemu-system-x86_64 --version
uname -a                           # host OS and kernel (Linux or FreeBSD)
```

Then the run itself and its bench lines:

```
MICH_KVM=1 MICH_QEMU_TIMEOUT=90 make test64-netbench 2>&1 | tee run.log
grep 'Mich netbench:' run.log
```

Copy the whole `grep` output. The lines that matter are `loopback-udp`,
`wire-tx`, `wire-rr`, and `wire-udp-tx`; each carries its payload size and a
`cycles=` or `p50=/p99=` field. rdtsc counts reference cycles, so the `lscpu` base
frequency is what converts them to nanoseconds. Do three runs and keep all three
(the first is a warmup); report the median.

## In-guest module and CI

The guest bench module (`src/user64/netbench`) runs two passes. The loopback pass
times the full UDP socket path in-kernel and reports cycles per round-trip. The
wire pass then dials the host peer over virtio-net (the guest is the active side)
and measures the socket path across the real NIC, the host bridge, and the peer's
own stack, so the delta against loopback is the wire overhead.

The kernel spawns the module through the same probe hook as the dns and tls
probes, but recognises the `netbench` module and skips the driver-live-recovery
lab for that boot. The lab deliberately crashes and restarts a driver capsule
against a tight tick deadline; a resident bench task both perturbs that timing (a
spurious recovery panic) and skews the cycle counts, so the benchmark wants a
quiescent kernel. The `netbench` smoke profile is therefore gated on
boot-essential plus net plus netbench markers only, since the full init CI battery
(which is wired behind that same lab) stays covered by the `disk-test` and `dns`
profiles.

The wire pass opens one control connection to the peer and runs every test over
it. Bulk TX (`TX`) is the gate: it is one-directional, so it proves the connect,
the stream send path, and the byte-accurate reply in about a second even under
TCG. After the gate come the best-effort tests, all on the same connection:
round-trip latency (`RR`) first, then UDP bulk TX at two sizes. RR runs first
because, unlike UDP under slirp, it produces real numbers even here, and each UDP
probe spends a fixed peer-side quiescence window; running RR first keeps that dead
time from starving it. Under TCG each round through slirp costs seconds, so RR and
UDP may not finish before the emulator tears the guest down, and slirp delivers
datagrams lossily; both are silent rather than faked when that happens. On KVM the
round trips are cheap and the loss is low, which is where the RR and PPS numbers
are meant to be collected. The best-effort chain stops if a test desyncs the
shared stream, because reconnecting would hit the second-connection limitation the
single shared connection exists to avoid.

## Status

- [x] Host peer, protocol v1, self-tested over loopback (TX/RX/RR/UDP).
- [x] Guest bench module, loopback socket path.
- [x] Makefile targets and QEMU bench profile (`make test64-netbench`).
- [x] Guest-to-host socket path from the module to the peer, multiplexed over one
  connection (TCP TX gate, RR, UDP TX).
- [x] Guest-to-host UDP TX path (PPS) from the module; verified delivering over
  slirp under TCG (lossy) and expected clean on KVM.
- [x] `MICH_KVM=1` KVM run path through the smoke runner.
- [ ] Guest-to-host UDP receive (URX) and datagram RR (URR) from the module.
- [ ] tap/vhost topology automated, with the peer addresses made configurable.
- [ ] Reference numbers on real KVM hardware, collected and tabulated.
