# Netbench wire-tx bulk send stall (open)

Status: OPEN. Narrowed to a data-plane drop on the TAP + vhost KVM topology.
Last worked: 2026-09-27.

## Symptom

On the KVM tap + vhost topology only, the netbench wire-tx bulk send
(`WIRE_TX_TOTAL` = 64 MiB) intermittently stalls partway (~9-10 MB, roughly one
run in two) and reports:

```
Mich netbench: wire-tx SEND FAIL at=<~9-10e6> of=67108864
```

The loopback path, the round-trip and packet-rate phases, IPv6 lifecycle, and
the whole boot test suite all pass. The failure is a rare timing race, not a
fixed byte threshold: observed offsets vary (5.77 MB, 9.07 MB, 9.46 MB,
9.97 MB, 10.26 MB, 10.82 MB, 14.1 MB, 43.6 MB across runs).

## Repro

```
sudo MICH_KEEP_LOG=/tmp/netbench.log MICH_TAP_TCPDUMP=1 \
    MICH_QEMU_TIMEOUT=120 scripts/net-bench/kvm-tap-bench.sh
sudo grep -E "wire-tx|send stall|send broke|send state" /tmp/netbench.log
tail -80 netbench-tap.txt   # the wire capture (see "next step")
```

Run it 3-5 times; it does not fail every time.

## What is already FIXED this session (do not re-investigate)

- Event object layer is race-free: `event_signal` latches `signaled=1` when
  there is no waiter, hands off directly to one slot when there is, both under
  the same `irq_save`, single CPU. Proven, not the bug.
- Lost wakeup for UNSENT send-buffer data. `tcp_tick` is purely timer driven, so
  a send-buffer tail stranded after a coalesced ACK (flight back to zero, window
  open, no retransmit or persist timer armed) was invisible to it and the writer
  slept until the peer's 30s receive timeout. Fixed by `tcp_pending_send`
  (`src/net/tcp.c`) drained every interface tick from `net_interface_tick`
  (`src/net/net_interface.c`). Commit `9174e59`.
- False SEND FAIL from a wakeup-count budget. The guest bounded back-pressure by
  counting wakeups (`STREAM_WAITS`), but `socket_tcp_notify` fires on every
  inbound segment and every maintenance tick, so a spurious-wakeup burst spent
  the whole budget in a fraction of a second and failed a live connection. The
  guest now bounds by elapsed time with no accepted chunk (`SEND_STALL_TICKS`,
  `src/user64/netbench/main.c`). Commit `67aad0f`. This is why bulk runs now
  usually complete where they always failed before.

## Ruled out

- Deterministic byte-count / buffer-size threshold: offsets vary ~7.5x.
- Congestion-window growth or retransmission-table exhaustion: peer window stays
  65535, so flight is capped ~64 KB (~45 segments), well under any limit.
- Peer bug: the `state=7 (CLOSE_WAIT) ready=16 (HANGUP) err=0` signature is the
  peer closing after its 30s `SO_RCVTIMEO`, i.e. a SYMPTOM of the guest wedge,
  not a cause. peer.c is sound (child per connection, PDEATHSIG, RCVTIMEO).
- The `user fault ... task=init64-two` / `task=spawn64` lines are EXPECTED boot
  tests: init64/spawn64 deliberately execute `ud2` (`src/user64/init/main.c:266,
  356, 405`) and a read-only write (`:724`) to exercise fault containment. They
  appear in passing runs too. Neither netbench nor virtio-net ever faults.

## Root cause: CONFIRMED as a data-plane drop, not a wakeup bug

Decisive evidence from the guest-side diagnostic on a stalled run:

```
Mich netbench: send stall wakes=1002 ready=1
```

- `ready=1` = CONNECTED only (not WRITABLE): the send buffer was full for the
  entire 10s window and never freed a byte. Connection still ESTABLISHED.
- `wakes=1002` over ~10s = ~100 wakeups/second = one per ~10ms. The guest is
  woken, retries the send, is refused (buffer still full), and waits again.

The buffer is full of ALREADY-SENT-but-UNACKNOWLEDGED data (send_buffer_offset
== send_buffer_length), which is why the tick backstop (`tcp_pending_send`, gated
on UNSENT data) does not touch it. ~100 acks/second arrive but do not advance
send_unacknowledged, i.e. they are DUPLICATE ACKs: the peer is missing an
earlier segment and keeps re-acking the hole. That points at a single TX segment
dropped in the capsule / vhost TX path around 9-10 MB whose retransmits are also
dropped, so no cumulative ACK ever comes, the buffer never drains, and the writer
starves. This matches the long-standing suspicion (three earlier fixes -
idle-spin `158fb62`, IRQ-preempt, TX-doorbell `0fa9730` - never took on TAP +
vhost).

## Next step (one run should localize it)

The harness now captures the wire to `netbench-tap.txt` (fixed this session: the
old `-c 200` cap stopped during the handshake and never saw the stall). After a
failing run, read the tail and answer one question at the stall point:

- Guest keeps transmitting (retransmits of the same seq) but there is a gap and
  the peer only sends duplicate acks -> TX segment is being DROPPED on the guest
  -> capsule TX submit path.
- Peer sends fresh acks that the guest never reacts to -> RX ACK is being DROPPED
  on the guest -> capsule RX path / ack processing.

Then instrument that path. Prime suspects in `src/user64/virtio_net/main.c`:
submit_tx path (@479-511), tx completion tracking (@517-551),
`process_tx_batch` (@679), `process_tx_completions` (@707); and whether a TX
descriptor can be silently dropped under sustained back-to-back submits, or a
used-ring entry missed while IRQs are masked in `itr_drain`.

A good kernel-side confirmation would be a one-shot dump, when a connection's
send_unacknowledged has not advanced for N ticks while flight > 0, of:
send_unacknowledged, send_next (flight), send_window, congestion_window,
retransmission count, and the dup-ack counter - to prove dup-acks + failing
retransmit directly.

## Perf numbers captured (successful bulk runs, for reference)

64 MiB wire-tx, TAP + vhost, KVM. Cycles are host TSC (rdtsc under -cpu host, so
they include wall time spent blocked on ACKs, not pure CPU):

| bytes    | total cycles  | send cycles   | wait cycles |
|----------|---------------|---------------|-------------|
| 67108864 | 36035421452   | 36027094688   | 8326764     |
| 67108864 | 37228717958   | 37220090086   | 8627872     |
| 67108864 | 35281886044   | 35273122712   | 8763332     |

At an assumed 2.4 GHz that is ~15s for 64 MiB, i.e. ~35 Mbit/s single stream -
low, ACK-clocked, buffer-limited. Real per-CPU frequency must feed collect.py
(`--tsc-hz`) for a true figure. Throughput optimization is the perf phase after
this bug is closed; reference target from the web is Linux virtio single-stream
~12,600 Mbit/s.

## Commits shipped this session

- `67aad0f` net: bound netbench bulk send by stall time not wakeup count
- `15d267d` tests: keep netbench serial log via MICH_KEEP_LOG for inspection
- `898517b` net: report tcp state and error when netbench bulk send breaks
- `9174e59` net: drain acked-stranded tcp send buffers on the interface tick
- `6cdc096` kernel: name the faulting task in the user fault log
