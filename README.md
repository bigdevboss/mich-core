# Mich Core

Mich Core is an experimental modular hybrid kernel for desktop operating systems. It is written from scratch and does not use Linux, BSD, XNU, or Windows source code.

Version 0.1.0 includes an in-kernel virtual NIC with a cycle-accurate netbench and
optimizes the VNIC packet path: the kernel now spends **224 cycles per 64-byte
packet** and **328 cycles per 1500-byte packet** on the NIC abstraction
(RX, KVM single-CPU, min) — a 31% to 82% reduction over the unoptimized path.
It also includes a bounded VFS (ramfs, immutable bootfs, and persistent
blockfs), bootfs firmware loading, a static x86-64 POSIX filesystem facade,
graceful driver stop, and the networking stack (userspace `virtio-net`
capsule, modern virtio PCI, split virtqueues, interrupt-driven RX/TX, DHCPv4,
IPv4/IPv6, UDP/UDPv6, TCP, stream sockets).

x86-64 is the only supported architecture. AArch64 and RISC-V 64 are planned.

Mich Core is still a development kernel. Do not use it for production systems or important data.

## Design

Mich keeps process management, scheduling, memory ownership, IPC, capabilities, and hardware resource control inside a small kernel core. Services and drivers may run in userspace when isolation is more valuable than a kernel-only implementation.

The current rules are:

- Hardware access is represented by typed kernel objects.
- Every handle carries explicit rights.
- New hardware drivers should start in userspace.
- Kernel and userspace drivers use the same resource model.
- Driver failure must not stop unrelated processes.
- Driver teardown masks interrupts, disables bus mastering, revokes mappings, invalidates handles, and applies reset policy before restart.
- Failed teardown quarantines the device and prevents unsafe reuse.
- Packet hot paths use preallocated buffers and bounded queues.
- Per-packet allocation is avoided.
- Performance claims must distinguish QEMU measurements from real wire throughput.

The long-term target is a POSIX-oriented desktop system for x86-64, AArch64, and RISC-V 64.

### Static POSIX application profile (x86-64 v0)

Mich provides a bounded POSIX-oriented **source** compatibility profile for
statically linked applications built specifically for Mich on x86-64. It is not
Linux syscall or binary compatibility, a dynamic linker, or a bundled
shell/userland.

The kernel VFS remains intentionally in kernel space. The profile is a separate,
narrow per-task authority above VFS files and the POSIX FD/OFD layer; it does
not grant `CAP_VFS_ADMIN`, native raw-VFS access, mount administration,
task administration, driver/resource access, or hardware privilege. Only an
executable image carrying the POSIX-profile boot metadata is admitted. Native
fork preserves the profile and descriptor state; task teardown releases both.

The public headers are `<fcntl.h>`, `<unistd.h>`, `<sys/types.h>`,
`<sys/wait.h>`, `<sys/stat.h>`, and `<errno.h>`. The v0 interface provides:

```text
open, close, read, write, lseek
dup, dup2, fcntl(F_GETFD/F_SETFD)
stat, fstat
mkdir, rmdir, unlink
chdir, getcwd, truncate
fork, execve, _exit
getpid, getppid, waitpid(WNOHANG/blocking)
```

`O_CREAT`, `O_TRUNC`, `O_APPEND`, and `O_CLOEXEC` are supported. File
permissions are stored in ramfs and persistent blockfs. Because v0 has no
UID/GID model, every admitted profile is treated as the owner: only owner
`0400`, `0200`, and `0100` bits grant read, write, and directory-search access.
Group and other bits remain stored and are reported through `st_mode`, but do
not grant access. `O_APPEND` uses VFS-level append serialization.

The ABI uses bounded request records internally; public `read` and `write`
wrappers chunk larger transfers. Mich does not claim POSIX certification or
complete POSIX conformance. Pipes, `mmap`, polling, sockets, signals, threads,
terminal semantics, UID/GID, `umask`, `chmod`, ACLs, and Linux ABI
compatibility remain outside this v0 application profile.

## 0.1.0

### VNIC packet-path optimization

The VNIC's hot path is optimized for measured cycles per packet:

- **SIMD frame copy**: the RX frame copy uses SSE2 128-bit moves (`movdqu`),
  replacing a scalar byte loop. SSE2 is mandatory in x86-64, so no CPUID check.
- **Cached ring resource pointer**: the VNIC caches the RX/TX ring resource
  pointer at create, so the hot path skips the per-packet object lookup.
- **Cached pool state**: the VNIC caches the packet pool state at create, so
  the hot path skips the per-packet object lookup.

Together these reduce the VNIC's RX cost from 1780 to 328 cycles/packet at
1500 bytes (**−82%**) and from 326 to 224 cycles/packet at 64 bytes (**−31%**).

### Measured cycles per packet (KVM, single CPU)

The numbers below are the kernel-side cycles per packet (ring submission and
consumption, pool state transitions, descriptor handling, and the frame copy),
measured on an Intel Core i3-7100U at 2.40GHz under QEMU 11.1.1 with KVM and a
single CPU (native CPU, real TSC). Because the netbench hot path is in memory
(no MMIO), KVM single-CPU is a close proxy for bare metal. The TCG (emulated
CPU) numbers run 5-9x higher on the same machine and are not shown.

Cycle counts belong to the processor they were taken on, so reproduce them
before comparing. Build the test image, then boot it under KVM with one CPU:

```bash
make bin/x86_64/disk-test.img
( . scripts/uefi-firmware.sh
  mich_uefi_firmware
  trap 'rm -f "$uefi_vars"' EXIT
  qemu-system-x86_64 -enable-kvm -cpu host -smp 1 -machine q35 \
    -drive if=pflash,format=raw,readonly=on,file="$uefi_code" \
    -drive if=pflash,format=raw,file="$uefi_vars" \
    -drive file=bin/x86_64/disk-test.img,format=raw,if=none,id=esdisk \
    -device ide-hd,drive=esdisk,bootindex=1 \
    -m 128M -serial stdio -display none -no-reboot -nic none
) | grep netbench
```

| Direction | 64 B (min) | 512 B (min) | 1500 B (min) |
|-----------|-----------:|------------:|-------------:|
| RX        | 224        | 254         | 328          |
| TX        | 296        | 758         | 1746         |
| Echo      | 460        | 958         | 2024         |

For reference, the unoptimized path was 326 (64 B), 790 (512 B), and 1780
(1500 B) cycles/packet on RX — the 0.1.0 optimization is a 31% to 82%
reduction. The protocol stack (Ethernet → ARP → IP → TCP/UDP) adds on top of
these numbers; the VNIC is the NIC abstraction the stack sits on.

### Bounded VFS and blockfs

The x86-64 port initializes a kernel VFS on the same object and handle model as the rest of the kernel:

- `KOBJECT_VNODE` and `KOBJECT_DIRECTORY` nodes, `KOBJECT_FILE` open files, `KOBJECT_MOUNT` mount points
- Up to 64 nodes, 32 open files, and 8 mounts
- 31-character names, 255-character paths, 32 path components
- Regular files up to 4 KiB; native VFS I/O requests up to 512 bytes
- Generation counters on nodes and mounts, so stale handles fail lookup
- Stored mode bits on files and directories
- Serialized append transactions and unlink-open file lifetime semantics

The root of the tree is a ramfs directory. At boot, every boot module is
published read-only under `/boot` as a bootfs mount, up to 16 modules of 1 MiB
each. The bounded blockfs backend stores regular files, directories, parent
links, modes, and file data on block devices; mount reconstruction validates
persistent hierarchy and mount generations protect against stale open files.

Path resolution accepts absolute and relative paths from any start handle. `.`
and `..` are honored, `..` at the root stays at the root, and components longer
than 31 characters are rejected.

The userspace VFS ABI is handle based:

```text
155  root
156  create
157  lookup
158  open
159  read
160  write
161  truncate
162  stat
163  unlink
164  resolve path
165  create path
166  unlink path
```

`CAP_VFS_ADMIN` gates create, unlink, and path operations. Lookup of a bootfs node also requires the capability, so a non-admin process holds the root handle without write rights and reaches boot files through the firmware interface. This native handle ABI is distinct from the restricted POSIX filesystem profile described above.

### Firmware loading

Driver domains open firmware from the bootfs mount by name:

- Each manifest carries an allowlist of up to 4 firmware names
- Lookup is fixed to `/boot/<name>`
- The target must be a regular read-only bootfs file
- The driver receives a read-only file handle
- Teardown revokes the handle, so a crashed driver cannot keep the file open

### Graceful driver stop

A driver manifest may set the graceful stop flag. Without the flag, the supervisor force-tears a domain down. With the flag, the sequence is:

1. The supervisor sends a stop request to the driver's Driver Bridge endpoint and marks the domain STOPPING with a 32-tick grace period
2. The driver acknowledges the stop through the stop-ack syscall once it has drained its work
3. Teardown runs in the normal order: mask IRQs, quiesce the device, revoke mappings, apply the reset policy
4. If the driver does not acknowledge, the timeout path force-tears the domain at the deadline, and a failed teardown quarantines the domain

The in-kernel driver tests cover the acknowledged path and the timeout fallback.

### Experimental recovery laboratory

The x86-64 QEMU laboratory can select one pretrusted, independently linked
`virtio-net-safe.elf` artifact after a repeated exact primary-capsule crash
fingerprint. The selection is one-shot and fail-closed; it does not patch or
recompile driver code. See [Recovery Laboratory](docs/recovery-laboratory.md)
for the selection contract, failure boundaries, audit record, and QEMU proof
profiles.

### Network interface objects

`KOBJECT_NET_INTERFACE` represents a registered network interface with:

- Stable interface ID and generation
- Driver-domain ownership
- Unique name
- MAC address and MTU
- Link state
- IPv4 configuration
- IPv6 link-local and SLAAC addresses
- Waitable link events
- Route generation
- Automatic revoke after driver death

Interface states are:

```text
CREATED
DOWN
UP
QUIESCING
REVOKED
REMOVED
```

Routes tied to a revoked interface generation are deactivated automatically.

### Packet ownership

Network buffers come from preallocated packet page pools. Every buffer ID contains a generation and has one explicit owner state:

```text
FREE
RX
STACK
TX
TX_QUEUED
DRIVER_RX
DRIVER_TX
```

The kernel rejects stale IDs, invalid transitions, double completion, and access using the wrong ownership state.

The normal packet path does not allocate physical pages or kernel objects.

### Ethernet and ARP

The Ethernet layer supports:

- Destination MAC filtering
- Broadcast and multicast
- Promiscuous mode
- VLAN header detection
- EtherType dispatch
- Batch receive
- Malformed frame rejection

The ARP layer provides:

- Bounded neighbor cache
- Request and reply generation
- Expiration
- Rate limiting
- Conflict detection
- Poisoning checks
- Pending IPv4 transmit queues
- Retry after TX ring pressure

### IPv4 and ICMP

IPv4 includes:

- Header and checksum validation
- IHL and option bounds
- TTL validation
- Source and destination filtering
- Fragment rejection
- Protocol dispatch
- Header generation
- Connected and default routing

ICMPv4 includes:

- Echo Request and Reply
- Destination Unreachable
- Time Exceeded
- Port Unreachable generation for closed UDP ports
- Broadcast suppression
- Rate limiting

The QEMU test performs a real external ping through `virtio-net`.

### UDP and sockets

UDP supports:

- Mandatory IPv4 pseudo-header checksum validation when a checksum is present
- Bounded port bindings
- Ephemeral ports from 49152 through 65535
- Full-MTU payloads up to 1472 bytes
- Bounded receive queues
- Zero-copy internal receive ownership
- Queue overflow accounting
- Waitable socket events
- Loopback and external interfaces

The userspace socket ABI supports:

```text
create
bind
send-to
receive-from
wait
```

External socket tests send DNS requests through QEMU networking and validate the replies.

### DHCPv4

The userspace `virtio-net` capsule implements:

```text
Discover
Offer
Request
ACK
```

It parses and applies:

- IPv4 address
- Subnet mask
- Default gateway
- DNS server
- Lease time
- T1 renewal time
- T2 rebinding time

The lifecycle includes:

- Retry timer
- Exponential retry delay
- Bounded retry count
- Renewal
- Rebinding
- NAK handling
- Controlled driver restart after lease failure

### Modern virtio PCI

The x86-64 backend supports modern virtio PCI capabilities:

- Common configuration
- Notify configuration
- ISR configuration
- Device configuration
- 64-bit feature negotiation
- `VIRTIO_F_VERSION_1`
- Queue discovery
- Queue enable
- Device status lifecycle
- Stable device configuration reads using config generation
- PCI memory decoding and bus mastering

The driver resets the whole device before releasing queue DMA. It does not write `queue_enable = 0` unless the negotiated feature set permits individual queue reset.

### Split virtqueues

The split virtqueue implementation provides:

- Descriptor free list
- Multi-descriptor chains
- Atomic allocation rollback
- Available-ring publication
- Used-ring collection
- 16-bit ring wrap handling
- Release and acquire barriers
- 48-bit chain generations
- Stale completion rejection
- Used-length validation
- Queue failure state after corruption
- Reset with outstanding chains
- Kick suppression
- Batched completion collection

The current completion ABI returns up to 16 completions per syscall. The userspace driver uses a bounded batch budget of 8.

### Userspace `virtio-net` capsule

The first real userspace network driver performs:

- Driver Bootstrap validation
- PCI ownership validation
- Virtio feature negotiation
- Stable MAC and link status reads
- RX queue creation
- TX queue creation
- MSI-X vector assignment
- Driver Bridge IRQ binding
- RX buffer publication
- TX publication and completion
- Packet pool ownership transitions
- Interface registration
- Link state management
- Driver restart through the supervisor

External ping, UDP, and TCP probes live in a separate module. The driver loop only pumps queues, DHCP, and IPv6 DAD.

RX and TX use separate MSI-X vectors. The normal driver loop waits on both Driver Bridge IRQ events and kernel timer objects through wait-many.

### Interrupt-driven and batched I/O

The driver uses a hybrid model:

```text
MSI-X interrupt
      |
      v
bounded RX and TX batches
      |
      v
buffer refill and publication
      |
      v
wait-many
```

Current batching includes:

- Up to 8 RX completions per processing pass
- Up to 8 TX submissions per pass
- Up to 16 outstanding TX buffers
- One RX notify after a refill batch
- One TX notify after a publication batch
- Cycle counters for RX and TX paths

QEMU cycle measurements are diagnostic values. They are not wire-throughput claims.

### IPv6

The IPv6 core provides:

- Fixed-header validation
- Traffic class and flow label
- Payload length and hop limit checks
- Multiple local addresses
- Link-local addresses from modified EUI-64
- Solicited-node multicast
- Multicast filtering
- Bounded extension-header traversal
- Destination Options
- Hop-by-Hop Options
- Authentication Header bounds
- Fragment rejection
- Routing Header rejection until a safe policy exists

### ICMPv6 and NDP

ICMPv6 supports:

- IPv6 pseudo-header checksum
- Echo Request and Reply
- Destination Unreachable
- Packet Too Big
- Time Exceeded
- Parameter Problem

NDP supports:

- Neighbor Solicitation
- Neighbor Advertisement
- Source and Target Link-Layer Address options
- Router Solicitation
- Router Advertisement
- Prefix Information
- Bounded neighbor cache
- Neighbor aging
- Duplicate Address Detection
- Global SLAAC address creation
- Preferred and valid lifetimes
- Address deprecation
- Router lifetime expiration
- Repeated Router Solicitation

The QEMU test performs real DAD, receives a Router Advertisement, creates a SLAAC address, answers NDP for that address, and completes an external IPv6 ping.

### UDPv6 and IPv6 sockets

UDPv6 includes:

- Mandatory checksum
- IPv6 pseudo-header
- Address-specific bindings
- Ephemeral ports
- Bounded receive queues
- Waitable events
- Generation-safe binding IDs

The IPv6 datagram socket ABI supports:

```text
create
bind
send-to
receive-from
wait
```

The test environment confirms external UDPv6 transmission and the ICMPv6 error path. QEMU SLIRP does not provide a usable UDPv6 DNS endpoint in the current profile, so an external UDPv6 datagram reply is not claimed.

### TCP

The TCP core includes:

- IPv4 and IPv6 checksums
- Header and option validation
- MSS advertised as the retransmit and out-of-order payload bound
- Window Scale
- SACK Permitted, SACK blocks on ACKs, and two-block coalescing
- Timestamps
- Active open
- Passive open
- Three-way handshake
- Bounded listen backlog
- Accept queue
- Sequence wrap comparisons
- Send and receive buffers
- Out-of-order queue and merge
- Cumulative ACK
- Duplicate ACK tracking
- Fast retransmit foundation
- Congestion window and slow-start threshold
- RTT estimator
- Dynamic RTO
- Retransmission queue
- Retry exhaustion
- Zero-window persist timer
- FIN, CLOSE_WAIT, CLOSING, LAST_ACK, and TIME_WAIT
- EOF
- Close does not FIN over undelivered data; a zero-window detach sends RST
- RST validation
- Blind RST rejection
- Socket error state

TCP connection state is bounded. There are up to four interface TCP contexts and up to 64 connections per context.

### Stream socket ABI

TCP stream sockets support:

```text
create
connect
listen
accept
send
receive
state
wait
shutdown
SO_ERROR
```

Readiness flags include:

```text
CONNECTED
READABLE
WRITABLE
ACCEPT
HANGUP
ERROR
```

The external QEMU tests cover:

- Active connect
- Three-way handshake
- 32 sequential echo round trips
- Shutdown and FIN lifecycle
- Passive listen
- Host-to-guest connect
- Accept
- Passive echo
- Host-side payload validation

TCPv6 checksum, segment generation, active handshake, and stream receive are tested internally. External TCPv6 is not claimed because the current QEMU SLIRP build rejects IPv6 guest forwarding rules.

## Driver and hardware safety

The x86-64 driver model includes:

- PCI and PCIe ECAM discovery
- Architecture-neutral IOMMU domain and fault interface
- ACPI IVRS parsing and AMD-Vi unit discovery
- AMD-Vi device table, command buffer, event log, and completion command
- AMD-Vi DTE domains, page tables, hardware invalidation, suspend, and resume
- AMD-Vi registration through the generic IOMMU lifecycle
- ACPI DMAR parsing and Intel VT-d register discovery
- VT-d root, context, domain, and second-level translation tables
- VT-d root activation, cache invalidation, IOTLB invalidation, and translation enable
- Per-domain coherent DMA IOVA mapping, crash revoke, and restart restore
- VT-d fault record decoding and driver quarantine
- Physical forbidden-DMA rejection with a QEMU EDU device
- BAR sizing with decoding and bus mastering disabled
- MSI and MSI-X
- Multi-vector groups
- IRQ binding to events or Driver Bridge endpoints
- PCI Function Level Reset
- Required and optional reset policy
- Driver manifests
- Dependency graph and cycle rejection
- Automatic PCI binding
- Restart limits and backoff
- Device ownership
- Quarantine after failed teardown

MMIO and DMA mappings require:

```text
KRIGHT_READ | KRIGHT_MAP
```

A writable page-table mapping is created only if the handle also has `KRIGHT_WRITE`.

The first VT-d backend reserves eight isolated domains. Each domain has a 16 MiB IOVA window and eight bounded mappings. Coherent DMA resources receive IOVA addresses when VT-d is available. Crash and timeout teardown remove their page-table entries, and restart restores the same bounded mappings. Platforms with RMRR entries remain rejected until reserved-region ownership is implemented.

PCI configuration writes use native 8-bit, 16-bit, or 32-bit operations. A 16-bit write does not perform a 32-bit read-modify-write over adjacent write-one-to-clear status bits.

### Handle safety

Kernel handles use:

```text
8-bit slot
24-bit generation
```

There are 32 handle slots per process. A 40000-cycle stress test verifies that an old handle does not become valid again under the previous 15-bit generation boundary.

### IRQ and event synchronization

Driver Bridge queues and event waiter state are protected against local interrupt races with `irq_save()` and `irq_restore()`.

This prevents an MSI-X interrupt from corrupting a queue or causing a lost wakeup while userspace is reading notifications or registering a wait set.

## Support matrix

| Feature | x86-64 |
| --- | --- |
| --- | --- |
| UEFI boot through BigDevBoot | Yes |
| Ring 3 processes | Yes |
| Timer preemption | Yes |
| ELF userspace | ELF64 |
| E820 physical memory manager | Yes |
| PID generations | Yes |
| Exit, wait, kill, and reparenting | Yes |
| Fork and exec | Yes |
| Copy-on-write fork | Yes |
| Blocking and nonblocking IPC | Yes |
| Service registry and capabilities | Yes |
| Kernel objects and generated handles | Yes |
| Shared pages, SG lists, rings, completions, and timers | Yes |
| ACPI, PCI, and PCIe ECAM | Yes |
| Local APIC and I/O APIC | Yes |
| MSI and MSI-X | Yes |
| Userspace driver supervisor | Yes |
| Automatic PCI driver binding | Yes |
| Driver quarantine and reset policy | Yes |
| Packet pools and virtual NIC | Yes |
| Userspace `virtio-net` driver | Yes |
| VFS (ramfs, bootfs, and bounded blockfs) | Yes |
| Static POSIX application profile | Yes |
| Block layer and bounded blockfs | Yes |
| Firmware loading | Yes |
| Graceful driver stop | Yes |
| External IPv4 | Yes |
| DHCPv4 | Yes |
| External IPv6 and SLAAC | Yes |
| UDP and UDPv6 | Yes |
| TCP and stream sockets | Yes |
| Panic register dump | Yes |
| FPU context switching | FXSAVE and FXRSTOR |
| SMP | Yes, AP bring-up, per-CPU state, IPI and spinlocks |
| TLS 1.3 client | Yes, x25519 with AES-128-GCM |
| X.509 validation | Yes, chain, validity and host name |
| HTTPS request | Yes, verified against a real server |
| IOMMU | Intel VT-d and AMD-Vi coherent DMA |
| AArch64 | Planned |
| RISC-V 64 | Planned |

## Security model

Current protections include:

- Supervisor-only kernel mappings
- NX userspace stacks
- CR0 write protection
- ELF bounds and overlap validation
- W+X ELF segment rejection
- Checked userspace pointers
- PID generations
- 24-bit handle generations
- Atomic handle transfer rollback
- IPC deadlock detection and timeouts
- Event waiter synchronization against IRQ delivery
- Driver Bridge queue synchronization
- User fault containment
- Syscall return-state validation
- Kernel and syscall stack canaries
- PCI bus-master shutdown during teardown
- IRQ, MSI, and MSI-X masking before revoke
- MMIO and DMA mapping revoke
- PCI reset policy
- Quarantine after teardown failure
- Bounded packet, route, socket, neighbor, and TCP state
- Blind TCP RST rejection
- Bounded SYN backlog
- TCP parser mutation stress

Important limitations remain:

- AMD-Vi Event Log fault decoding needs a physical fault integration test
- VT-d RMRR ownership is not implemented
- Devices without a supported IOMMU remain trusted for DMA
- No SMP synchronization model
- No SMEP or SMAP
- No KASLR
- No complete x86-64 kernel W^X
- The low identity mapping remains active
- PCI devices can DMA outside assigned buffers without an IOMMU
- Public ABIs may change before 1.0.0

Report security issues privately when possible:

```text
mich-licensing@protonmail.com
```

Do not publish an unpatched vulnerability before the maintainer has had reasonable time to investigate it.

## What is not here yet

Mich Core 0.1.0 does not include:

- A general-purpose production filesystem: ramfs, bootfs, and blockfs are
  bounded implementations with deliberately small limits
- USB
- Audio
- A desktop or shell
- POSIX pipes, `mmap`, polling, signals, or a complete POSIX runtime ABI
- POSIX certification, complete POSIX conformance, or Linux binary/syscall ABI
  compatibility
- Independently written Linux Kernel API compatibility headers
- SMP
- IOMMU-backed DMA isolation
- Power management
- A higher-half kernel
- Complete kernel W^X, SMEP, SMAP, or KASLR
- Real high-speed NIC measurements
- Confirmed external TCPv6 through the current QEMU backend

The disk images are test systems, not installable operating systems.

## Build requirements

Use a Linux host with:

- GNU Make
- GCC
- GNU binutils
- NASM
- Python 3
- QEMU for x86
- OVMF, the UEFI firmware QEMU boots from
- GNU coreutils

On Debian or Ubuntu:

```bash
sudo apt update
sudo apt install build-essential gcc-multilib binutils nasm python3 qemu-system-x86 ovmf coreutils
```

The build does not download dependencies.

Mich boots through UEFI only. The runners look for OVMF in the usual
Debian and Fedora locations; set `MICH_OVMF_CODE` and `MICH_OVMF_VARS` if
yours lives somewhere else.

## Build

```bash
git clone https://github.com/bigdevboss/mich-core.git
cd mich-core
make -j2
```

Generated files are written under `bin/`.

## Run

```bash
make run64
```

Kernel logs go to the serial console.

## Test

Run the normal test profile:

```bash
make test64
```

Run dedicated x86-64 profiles:

```bash
make test64-highmem
make test64-hardware
make test64-msi
make test64-msi-restart
make test64-msi-circuit
make test64-msi-recovery
make test64-msi-restart-stability
make test64-msi-circuit-stability
make test64-msi-recovery-stability
make test64-pcie
make test64-iommu
make test64-amd-iommu
make test64-panic
```

Run the complete release check:

```bash
make release-check
```

The release check performs a clean build and runs:

- x86-64 smoke test with 128 MiB
- x86-64 high-memory test with 768 MiB
- Hardware-destructive interrupt profile
- MSI and MSI-X hardware programming
- Modern virtio PCI tests
- Real userspace `virtio-net`
- DHCP, IPv4, IPv6, UDP, TCP, and socket integration
- PCIe ECAM on Q35
- ACPI DMAR and Intel VT-d register discovery
- ACPI IVRS and AMD-Vi register discovery
- AMD-Vi device table, command processing, DTE, and page-table lifecycle
- VT-d context and second-level translation table lifecycle
- VT-d root activation, invalidation, and translation enable
- Production driver IOVA assignment and revoke lifecycle
- VT-d fault decode and quarantine routing
- Physical forbidden-DMA rejection through QEMU EDU
- Intentional kernel panic profile
- Version and formatting checks

The normal boot profile does not program discovered MSI or MSI-X devices. Destructive hardware tests require an explicit hardware profile.

## Test coverage

The suites cover:

- ELF32 and ELF64 validation
- Address-space and physical-page accounting
- High-memory identity-map regression
- Process lifecycle and PID generations
- IPC modes, timeout, deadlock, and death notification
- Handle rights, transfer, revoke, and generation stress
- VFS nodes, mounts, paths, modes, append serialization, unlink-open semantics,
  and root escape protection
- Persistent blockfs file and directory hierarchy reconstruction, mode restore,
  live-open unmount protection, and stale-generation rejection
- POSIX FD/OFD lifetime, `FD_CLOEXEC`, cwd/detached-cwd behavior, owner-mode
  enforcement, the userspace POSIX filesystem facade, the process facade
  (`fork`/`execve`/`waitpid`), and static application fixtures (`posixapp`,
  `posixdemo`)
- Firmware allowlist, lookup, and crash handle revocation
- Graceful driver stop, stop timeout fallback, and stop-ack path
- Event and Driver Bridge IRQ race protection
- PCI and PCIe discovery
- MSI and MSI-X programming
- Driver manifests, dependency cycles, fallback, and restart
- Driver teardown, reset, quarantine, and resource accounting
- Shared pages, SG rollback, rings, completions, timers, and wait-many
- Packet-pool exhaustion and stale IDs
- Ethernet, ARP, IPv4, ICMP, UDP, and routing
- DHCP retry, renewal, rebinding, and lease state
- IPv6 extension bounds, DAD, NDP, RA, SLAAC, and ICMPv6
- UDPv6 checksum and binding state
- TCP options, checksum, sequence handling, and wrap comparisons
- TCP retransmission, RTT, RTO, persist, FIN, CLOSING, EOF, and TIME_WAIT
- Active and passive stream sockets
- External active and passive TCP echo
- TCP connection churn
- Bounded SYN flood handling
- 4096 TCP parser mutations
- Network parser mutations
- Netbench baseline report: rx, tx, and echo at 64, 512, and 1500 bytes
- Panic diagnostics

## Repository layout

```text
LICENSE
Makefile
README.md
mkuefi64.py
scripts/
src/
```

Important source modules:

```text
src/core/                 types, boot info, serial API, and page memory
src/process/              tasks, scheduler, fork/exec, IPC, capabilities, POSIX FD/profile/process facade
src/user64/lib/posix.c    static POSIX userspace wrappers and errno
src/user64/posixdemo/     static POSIX application fixture (step-6 showcase)
src/user64/include/       Mich APIs and the bounded POSIX public headers
src/objects/              kernel objects, resources, events, rings, completions
src/net/                  full protocol stack (ARP through TCP, sockets)
src/driver/               driver domains, supervisor, manager, virtio ABI
src/fs/                   VFS and firmware loading
src/objects/object.c      kernel objects and handle tables
src/objects/resource.c    MMIO, IRQ, DMA, PCI, pages, and SG resources
src/objects/iommu.c       architecture-neutral IOMMU lifecycle
src/arch/x86_64/platform/vtd64.c   Intel VT-d translation backend
src/arch/x86_64/platform/amd_iommu64.c   AMD-Vi discovery backend
src/driver/driver.c       kernel driver module core
src/driver/driver_supervisor.c   userspace driver domains and crash recovery
src/driver/driver_manager.c   manifests, dependencies, matching, and binding
src/objects/bridge.c      Driver Bridge notifications
src/objects/event.c       events and wait-many
src/objects/ring.c        shared descriptor rings
src/objects/completion.c  asynchronous completion objects
src/net/net_buffer.c      packet page pools
src/net/vnic.c            virtual benchmark NIC
src/net/net_interface.c   interface registry and external transports
src/net/ethernet.c        Ethernet parser and dispatch
src/net/arp.c             ARP neighbor cache
src/net/ipv4.c            IPv4 parser and dispatch
src/net/icmp.c            ICMPv4
src/net/udp.c             UDPv4
src/net/ipv6.c            IPv6 parser and address state
src/net/icmpv6.c          ICMPv6 and NDP
src/net/udpv6.c           UDPv6
src/net/tcp.c             TCP transport core
src/net/socket.c          datagram and stream socket objects
src/fs/vfs.c              VFS nodes, mounts, and path resolution
src/fs/firmware.c         driver firmware loading from bootfs
src/arch/x86_64/drivers/virtio_pci.c modern virtio PCI and split virtqueues
src/user64/virtio_net/    userspace virtio-net capsule
```

## Network performance direction

The performance goal is measured cycles per packet, not unsupported throughput claims.

The hot path should avoid:

- Per-packet allocation
- Global locks
- Payload copies
- Shared writable counters
- Unnecessary callbacks
- Cross-core cache-line movement

Future real-hardware testing should report:

- Packet size
- Packets per second
- Gbit/s
- Packet loss
- p50 and p99 latency
- CPU model and frequency
- Core count
- NIC model
- PCIe generation
- NUMA placement
- Queue count
- RSS configuration
- Offload settings

QEMU cycle measurements are useful for regressions. They are not wire-rate results.

### Netbench baseline

The test build includes a netbench baseline on the virtual benchmark NIC. It measures the kernel ring and pool paths for three directions (rx, tx, and echo) at 64, 512, and 1500 byte frames. Each direction and size runs 512 warmup packets, then 4096 sampled packets timed per packet with rdtsc, and reports min, average, p50, and p99 cycles per packet on the serial console.

The vnic harness measures the kernel-side path: ring submission and consumption, pool state transitions, and descriptor handling. Frame copies model the DMA payload transfer. p99 values include any tick or scheduler interference that lands during a run.

After the 0.1.0 packet-path optimization (SIMD frame copy, cached ring resource
pointer, cached pool state), the netbench reports the following min cycles per
packet on the i3-7100U reference machine described above (KVM, single CPU,
real TSC, a close proxy for bare metal because the hot path is in memory with
no MMIO):

| Direction | 64 B | 512 B | 1500 B |
|-----------|-----:|------:|-------:|
| RX        | 224  | 254   | 328    |
| TX        | 296  | 758   | 1746   |
| Echo      | 460  | 958   | 2024   |

The unoptimized RX baseline was 326 / 790 / 1780 cycles per packet at 64 / 512
/ 1500 bytes - a 31% / 68% / 82% reduction. TCG (emulated CPU) numbers run
5-9x higher on the same machine and are useful only for spotting relative
regressions, never as a figure to quote.

## TLS 1.3

The kernel ships a TLS 1.3 client and can complete a handshake with an
ordinary server, validate its certificate chain and send an HTTPS request.
`make test64-tls` runs exactly that against a local `openssl s_server`.

The primitives live in `src/crypto/` and are checked against published test
vectors rather than against themselves: FIPS 180-4 and RFC 4231 for SHA-256
and HMAC, RFC 5869 for HKDF, the GCM specification test cases, RFC 7748 for
x25519, FIPS 186-4 for ECDSA P-256, and the self-signatures of ISRG Root X1
and DigiCert Global Root G2 for RSA. The key schedule and the record layer are
checked against the byte-for-byte handshake published in RFC 8448, and the
certificate parser against the 150 certificates in a system trust store plus
the live chain served by google.com.

### What it does not do

This is a working client, not a replacement for a TLS library. Missing on
purpose, and worth knowing before trusting it with anything:

- **No revocation checking.** Neither OCSP nor CRL. A certificate that has
  been revoked still validates.
- **No session resumption and no 0-RTT.** Every connection pays for a full
  handshake.
- **No KeyUpdate**, so a connection cannot rekey and is bounded by the record
  sequence number.
- **One group and one cipher suite**: x25519 and `TLS_AES_128_GCM_SHA256`. A
  server that insists on anything else is refused rather than negotiated with.
- **No P-384**, so an ECDSA chain on that curve cannot be verified. RSA and
  ECDSA P-256 chains work.
- **No client certificates.**
- Three roots are compiled in: ISRG Root X1, DigiCert Global Root G2 and
  GTS Root R1.

## Versioning

Mich Core uses semantic versioning for public releases.

The ABI may change during the `0.x` series. Version 1.0.0 will mark the first release intended to provide stable public kernel interfaces on supported architectures.

## Contributions

Bug reports, test results, design discussion, and documentation fixes are welcome.

Discuss large changes before opening a pull request. Do not submit proprietary code, leaked material, or code with an incompatible license.

Contributions are accepted under GPLv3.

## License

All Mich Core code is available under the [GNU General Public License v3.0](LICENSE).

Commercial dual licensing is also available under terms discussed separately by email: [mich-licensing@protonmail.com](mailto:mich-licensing@protonmail.com) or [shiftluckyxd@mail.ru](mailto:shiftluckyxd@mail.ru).

See [LICENSE](LICENSE) for the GPLv3 terms.
