# Recovery Laboratory

## Status

This is an experimental x86-64 QEMU recovery laboratory. It proves a bounded
selection and restart path for one real userspace `virtio-net` driver capsule.
It is not a production driver-repair system.

The laboratory selects only a boot-provisioned, pretrusted alternate artifact.
It does not perform signature verification, firmware-version attestation,
automatic binary patching, runtime source compilation, instruction rewriting,
or socket data-path bypass.

## Artifact boundary

The primary image is the normal `virtio-net` capsule. The alternate image is
`virtio-net-safe.elf`.

`virtio-net-safe.elf` is linked independently from `crt0.o`, `syscall.o`, and
its own `virtio_net_safe.o`. It has no primary capsule or probe object as a
link input. It retains only the kernel object, capability, syscall, PCI,
virtqueue, IRQ, RX/TX, static IPv4, and outbound TCP echo contracts needed for
recovery proof.

The safe capsule deliberately omits primary restart and circuit test logic,
IPv6 lifecycle, passive listener handling, UDP and socket probes, adaptive
interrupt moderation, and packet metrics.

## Selection contract

The private recovery catalog is fixed in the booted kernel image. For the QEMU
virtio-net laboratory entry, fallback is eligible only when all of these hold:

1. The PCI device is vendor `0x1AF4`, device `0x1000`.
2. The crash is a userspace exception with exit code `134`, vector `6`, error
   `0`, address `0`, and the designated primary-capsule `ud2` RIP.
3. The trigger is the crash circuit, not an arbitrary restart-limit event.
4. The primary fallback has not already been used.
5. Normal teardown completed. A failed teardown quarantines the domain before
   fallback selection is considered.

The primary test capsule has one shared source-level `ud2` in
`restart_test_poll()`. No external fault helper is introduced for recovery
selection. The second identical test fault occurs after fresh `eth0`
registration and before DHCP so the two faults share the approved fingerprint.

A matching selector changes the domain to the alternate image for one bounded
backoff and launch attempt. A mismatch, unavailable fallback, or failed
alternate launch never relaxes capability rights, device reset policy, IOMMU
mappings, DMA ownership, generation checks, or the existing circuit breaker.

## Teardown and failure ordering

Before any restart or fallback, the supervisor uses the established order:

1. Mask IRQs.
2. Quiesce the device.
3. Revoke driver resources and close handles.
4. Apply the configured reset policy.
5. Recreate the Driver Bridge endpoint for the fresh generation.

If any teardown operation fails, the domain is quarantined and fallback is not
considered. IOMMU faults use their existing quarantine path and do not select a
recovery artifact. A failed alternate launch leaves the domain failed; it does
not retry the artifact or revert to a broader selection rule.

## Audit record

Each fallback consideration writes one bounded, kernel-internal
`driver_recovery_audit` record in the domain status. It contains:

- the recovery trigger;
- the candidate image, capabilities, and argument;
- the exact crash passport, including generation and ticks;
- one outcome: `SELECTED`, `UNAVAILABLE`, or `SELECTOR_MISMATCH`.

The record is cleared with the domain's fallback state during initialization
and final release. It is not exposed through a userspace syscall or a public
recovery ABI.

## QEMU proof profiles

The individual targets preserve their normal fixed budgets and marker checks:

```sh
make -j2 test64
make -j2 test64-hardware
make -j2 test64-msi
make -j2 test64-msi-restart
make -j2 test64-msi-circuit
make -j2 test64-msi-recovery
```

`test64-msi-recovery` requires both primary fault generations, the exact
selector order, the independent safe-capsule readiness markers, static IPv4,
and a host-observed outbound TCP echo after artifact selection. It also
requires the in-kernel recovery audit self-test marker before the live
baseline.

The explicit stability targets run independent whole-QEMU boots. They are not
retries: every boot must pass, and the first failure stops the campaign.
Existing timeout values and smoke assertions are unchanged.

```sh
make -j2 test64-msi-restart-stability
make -j2 test64-msi-circuit-stability
make -j2 test64-msi-recovery-stability
```

Each stability target runs three boots by default. Set
`RECOVERY_STABILITY_RUNS` to another positive count for an explicit campaign.
On failure, the runner preserves the failing serial output at
`bin/x86_64/<profile>-stability-failure.log`, or at the path supplied through
`MICH_STABILITY_FAILURE_LOG`. It also preserves the associated host passive
peer timeline at the same path with `.passive` appended.

The restart, circuit, and recovery primary capsules emit bounded
`Mich virtio-net: timing <phase> ticks=0x...` diagnostics around the passive
peer exchange and recovery transition. Passive phases include listener, accept,
the first receive poll, payload availability, an empty-receive wait, its first
wake, echo, and close. If the listener is still unaccepted at its first
existing IPv6-timer wake, one listener `passive snapshot` records its
state/readiness/error/EOF and RX completion progress from listener creation.
On a first successful empty receive and on the first post-wake receive attempt,
bounded `passive snapshot` records expose the accepted socket
state/readiness/error/EOF and whether RX completion counters advanced between
those points. A separate bounded raw ingress witness marks the first untagged
IPv4/TCP SYN, post-SYN ACK, and payload frame for the passive port after
virtio-header validation but before core dispatch; it proves only receipt in
the guest RX completion, not TCP-core acceptance. They are test-profile
telemetry, not new success markers or a timing guarantee. For an individual
smoke run,
setting `MICH_QEMU_PASSIVE_TRACE=<path>` records the host monotonic-time
side of the passive proof without changing its exact echo requirement.

## Laboratory completion criteria

The laboratory v1 claim requires a clean rebuild, the focused static checks,
the individual recovery profiles, and an explicit stability campaign with no
failed boot. It remains an experimental QEMU result. Hardware compatibility,
firmware identity, administrator promotion workflow, offline candidate
synthesis, and production trust provisioning are separate work.
