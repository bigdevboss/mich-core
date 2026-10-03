# Mich Core system calls

This is the system call reference for anyone building an operating system,
driver, or userspace on top of the Mich kernel. It lists every call the kernel
dispatcher accepts, grouped by subsystem, with its number and a one-line
description.

The authoritative sources are the dispatcher in
`src/arch/x86_64/kernel/syscall_dispatch.c` and the userspace wrappers in
`src/user64/lib/syscall.asm`. This document tracks those.

## Calling convention (x86-64)

Mich uses the `syscall` instruction:

- The call number goes in `RAX`.
- Up to three arguments go in `RDI`, `RSI`, `RDX`, in that order.
- The result is returned in `RAX`.
- `RCX` and `R11` are clobbered by `syscall`, as always on x86-64.

By convention a call returns `0` or a non-negative value on success (a handle, a
count, a tick, an address, a byte total) and a negative or `-1`
(`0xFFFFFFFFFFFFFFFF`) value on failure. Calls fail closed: a missing capability,
a missing handle right, or a bad pointer returns an error rather than doing part
of the work.

Handles are indices into the calling task's handle table. Every handle carries
explicit rights (read, write, control, map, wait, signal, transfer), and calls
check the rights they need. Object-creating calls return a handle; the caller
closes it with `handle_close` (33).

Many calls are capability-gated. Task creation needs `CAP_TASK_ADMIN`; raw
physical, PCI, and NVMe access needs `CAP_RESOURCE_ADMIN`. Driver-facing network
and block calls require the caller to be a registered driver domain.

## Notes on numbering

- The number space is not contiguous. Unlisted numbers are reserved.
- Two ABIs share one dispatcher: the native Mich ABI (below) and a POSIX profile
  (186-209 and 217) used by the static libc. The socket stream file calls hold
  210 and 211, and their driver-domain helpers hold 212 through 216, so the
  POSIX profile continues at 217. **Number 186 is shared** - the native block
  capsule interface create (`SYS_BLOCK_INTERFACE_CREATE`) and POSIX `open`
  (`POSIX_SYSCALL_OPEN`). The dispatcher disambiguates by caller: a driver domain
  gets the block-capsule register, and any other task falls through to POSIX
  `open`. A task is only ever one or the other, so both meanings coexist at 186.

---

## Native Mich ABI

### Console and self-test

| Num | Name | Description |
|----|------|-------------|
| 1 | write | Write a NUL-terminated string to the serial console. |
| 4 | (self-test) | Emit the syscall/sysret self-test marker. |

### Inter-process messaging (ports)

| Num | Name | Description |
|----|------|-------------|
| 5 | send | Send a message to a port, blocking if it is full. |
| 6 | recv | Receive a message from any sender. |
| 7 | send_nb | Send without blocking; fail if the port is full. |
| 24 | recv_from | Receive a message from a specific sender. |
| 31 | send_timeout | Send with a tick deadline. |

### Tasks and scheduling

| Num | Name | Description |
|----|------|-------------|
| 11 | memfree | Free physical page count (needs `CAP_RESOURCE_ADMIN`). |
| 12 | fork | Duplicate the current task. |
| 13 | exec | Replace the current image from a loaded object. |
| 14 | exit | Terminate the current task with a status code. |
| 15 | wait | Wait for a child to exit and reap it. |
| 16 | getpid | Return the current task id. |
| 17 | kill | Terminate a child (or any task with `CAP_TASK_ADMIN`). |
| 23 | yield | Yield the CPU to the scheduler. |
| 32 | spawn | Create a new task (needs `CAP_TASK_ADMIN`); accepts `SPAWN_FLAG_SUSPENDED`. |
| 216 | task_resume | Start a task that was spawned suspended. |

### Capabilities

| Num | Name | Description |
|----|------|-------------|
| 20 | cap_get | Read the current task's capability bits. |
| 21 | cap_drop | Clear capability bits from the current task. |
| 22 | cap_grant | Grant capabilities to a child (needs `CAP_TASK_ADMIN`). |

### Service registry

| Num | Name | Description |
|----|------|-------------|
| 18 | service_register | Register the current task under a service id. |
| 19 | service_lookup | Look up the task registered for a service id. |

### Handles

| Num | Name | Description |
|----|------|-------------|
| 33 | handle_close | Close a handle. |
| 38 | handle_duplicate | Duplicate a handle into another task with given rights. |
| 59 | handle_transfer_batch | Transfer several handles to another task in one call. |

### Events and waiting

| Num | Name | Description |
|----|------|-------------|
| 34 | event_create | Create an event object. |
| 35 | event_wait | Wait on an event. |
| 36 | event_signal | Signal an event. |
| 37 | event_reset | Reset an event. |
| 58 | event_wait_timeout | Wait on an event with a tick deadline. |
| 81 | completion_wait | Wait on a completion object. |
| 85 | timer_wait | Wait on a timer object. |
| 86 | wait_many | Wait on several waitable objects at once. |

### Bridge endpoints

| Num | Name | Description |
|----|------|-------------|
| 39 | bridge_create | Create a bridge notification endpoint. |
| 40 | bridge_wait | Wait for a notification on an endpoint. |
| 41 | bridge_read | Read a pending notification from an endpoint. |

### Interrupts

| Num | Name | Description |
|----|------|-------------|
| 42 | irq_bind | Bind an IRQ resource to an endpoint. |
| 43 | irq_unbind | Unbind an IRQ resource. |
| 44 | irq_set_mask | Mask or unmask an IRQ. |
| 45 | irq_wait | Wait for an IRQ notification (shares the endpoint wait path). |
| 52 | irq_open | Open a platform or legacy IRQ object. |

### PCI

| Num | Name | Description |
|----|------|-------------|
| 46 | pci_count | Count enumerated PCI devices. |
| 47 | pci_open | Acquire a PCI device object (needs `CAP_RESOURCE_ADMIN`). |
| 48 | pci_bar_open | Open a device BAR as an MMIO resource. |
| 175 | pci_config_read8 | Read one byte of PCI config space. |
| 176 | pci_config_read16 | Read two bytes of PCI config space. |
| 177 | pci_config_read32 | Read four bytes of PCI config space. |
| 178 | pci_set_command | Write the PCI command register. |

### MSI and MSI-X

| Num | Name | Description |
|----|------|-------------|
| 50 | msi_open | Create an MSI interrupt for a PCI device. |
| 51 | msix_table_open | Create the MSI-X table resource for a device. |
| 53 | msix_irq_open | Create an MSI-X vector from a table. |
| 61 | msi_group_open | Create a group of MSI interrupts. |
| 62 | msix_group_open | Create a group of MSI-X vectors. |

### DMA and MMIO mapping

| Num | Name | Description |
|----|------|-------------|
| 49 | dma_allocate | Allocate a DMA-capable memory resource. |
| 54 | mmio_map | Map an MMIO resource into the address space. |
| 55 | dma_map | Map a DMA resource into the address space. |
| 56 | resource_unmap | Unmap a previously mapped resource. |
| 57 | resource_length | Query the byte length of a resource. |
| 215 | resource_physical | Bus/physical address at a byte offset in an owned resource. |

### Shared memory and pages

| Num | Name | Description |
|----|------|-------------|
| 63 | page_create | Create an owned page resource. |
| 64 | shared_memory_create | Create a shareable memory resource. |
| 65 | page_map | Map a page or shared-memory object. |
| 66 | page_pin | Pin a page resource so it cannot be trimmed. |
| 67 | page_unpin | Unpin a page resource. |
| 68 | page_revoke | Revoke a page resource. |

### Scatter-gather lists

| Num | Name | Description |
|----|------|-------------|
| 69 | sg_create | Build a scatter-gather list from page entries. |
| 70 | sg_revoke | Revoke a scatter-gather list. |

### Rings (shared queues)

| Num | Name | Description |
|----|------|-------------|
| 71 | ring_create | Create a shared ring resource. |
| 72 | ring_map | Map a ring's backing memory. |
| 73 | ring_submit | Submit an entry to a ring. |
| 74 | ring_consume | Consume an entry from a ring. |
| 75 | ring_revoke | Revoke a ring resource. |

### Completions

| Num | Name | Description |
|----|------|-------------|
| 76 | completion_create | Create a completion object. |
| 77 | completion_begin | Begin a completion request. |
| 78 | completion_finish | Finish a completion request. |
| 79 | completion_cancel | Cancel a completion request. |
| 80 | completion_poll | Poll a completion request. |

### Timers

| Num | Name | Description |
|----|------|-------------|
| 82 | timer_create | Create a timer object. |
| 83 | timer_arm | Arm a timer. |
| 84 | timer_cancel | Cancel a timer. |

### Virtual NIC (in-kernel)

| Num | Name | Description |
|----|------|-------------|
| 87 | vnic_create | Create a virtual NIC with pool and RX/TX rings. |
| 88 | packet_pool_map | Map a VNIC packet pool's backing memory. |
| 89 | vnic_inject | Inject a frame into the VNIC. |
| 90 | vnic_receive | Receive a frame. |
| 91 | vnic_release_rx | Return an RX buffer to the pool. |
| 92 | vnic_acquire_tx | Acquire a TX buffer. |
| 93 | vnic_submit_tx | Submit a TX buffer for sending. |
| 94 | vnic_drain_tx | Drain completed TX buffers. |
| 95 | vnic_benchmark | Run the in-kernel VNIC benchmark. |
| 96 | vnic_revoke | Revoke a VNIC. |

### Datagram sockets (UDP)

| Num | Name | Description |
|----|------|-------------|
| 97 | socket_create | Create a UDP/IPv4 socket. |
| 98 | socket_bind | Bind a socket to a local port. |
| 99 | socket_send_to | Send a datagram. |
| 100 | socket_receive_from | Receive a datagram. |
| 101 | socket_wait | Wait for socket readiness. |
| 137 | socket_ipv6_create | Create a UDP/IPv6 socket. |
| 138 | socket_ipv6_bind | Bind an IPv6 socket. |
| 139 | socket_ipv6_send_to | Send an IPv6 datagram. |
| 140 | socket_ipv6_receive_from | Receive an IPv6 datagram. |

### Stream sockets (TCP)

| Num | Name | Description |
|----|------|-------------|
| 144 | socket_stream_create | Create a TCP socket. |
| 145 | socket_stream_connect | Connect to a remote endpoint. |
| 146 | socket_stream_send | Send on a stream. |
| 147 | socket_stream_receive | Receive from a stream. |
| 148 | socket_stream_state | Query stream connection state. |
| 149 | socket_stream_shutdown | Shut down a stream. |
| 150 | socket_stream_listen | Listen for IPv4 connections. |
| 151 | socket_stream_accept | Accept a pending connection. |
| 154 | socket_stream_take_error | Read and clear the pending socket error. |
| 167 | socket_stream_listen_ipv6 | Listen for IPv6 connections. |
| 210 | socket_stream_send_file | Send file contents over a stream (zero-copy). |
| 211 | socket_stream_receive_file | Receive stream data into a file. |

### Network interfaces (driver domains)

| Num | Name | Description |
|----|------|-------------|
| 102 | net_interface_create | Create a network interface for a driver domain. |
| 103 | net_interface_set_link | Set interface link state. |
| 104 | net_interface_get_info | Read interface information. |
| 105 | net_interface_revoke | Revoke a network interface. |
| 118 | driver_acquire_rx | Acquire an RX descriptor. |
| 119 | driver_receive | Receive one frame from the interface. |
| 120 | driver_dequeue_tx | Dequeue a frame to transmit. |
| 121 | driver_complete_tx | Complete a transmitted frame. |
| 122 | driver_release_rx | Release an RX descriptor. |
| 170 | driver_acquire_rx_batch | Acquire a batch of RX descriptors. |
| 171 | driver_receive_batch | Receive a batch of frames. |
| 172 | driver_dequeue_tx_batch | Dequeue a batch to transmit. |
| 173 | driver_complete_tx_batch | Complete a batch of transmitted frames. |
| 123 | configure_ipv4 | Configure the interface IPv4 address. |
| 124 | send_echo | Send an ICMPv4 echo request. |
| 125 | echo_replies | Read received ICMPv4 echo replies. |
| 126 | send_udp_probe | Send a UDP probe. |
| 127 | poll_udp_probe | Poll a UDP probe. |
| 130 | ipv6_start | Start IPv6 on the interface. |
| 131 | ipv6_complete_dad | Complete IPv6 duplicate address detection. |
| 132 | ipv6_get_info | Read IPv6 interface information. |
| 133 | ipv6_send_echo | Send an ICMPv6 echo request. |
| 134 | ipv6_echo_replies | Read received ICMPv6 echo replies. |
| 135 | udpv6_start_probe | Start a UDPv6 probe. |
| 136 | udpv6_poll_probe | Poll a UDPv6 probe. |
| 141 | ipv6_maintenance | Run interface timers and maintenance. |
| 142 | tcp_probe_start | Start a TCP probe. |
| 143 | tcp_probe_poll | Poll a TCP probe. |
| 152 | tcpv6_probe_start | Start a TCPv6 probe. |
| 153 | tcpv6_probe_poll | Poll a TCPv6 probe. |

### Driver domains and firmware

| Num | Name | Description |
|----|------|-------------|
| 60 | driver_bootstrap | Register the caller as a driver domain. |
| 168 | firmware_open | Open a firmware blob (driver domain, if allowed). |
| 169 | driver_stop_ack | Acknowledge a driver stop request. |

### Virtual filesystem

| Num | Name | Description |
|----|------|-------------|
| 155 | vfs_root | Get a handle to the VFS root. |
| 156 | vfs_create | Create a node under a directory handle. |
| 157 | vfs_lookup | Look up a node by name under a directory. |
| 158 | vfs_open | Open a vnode as a file handle. |
| 159 | vfs_read | Read from a file handle. |
| 160 | vfs_write | Write to a file handle. |
| 161 | vfs_truncate | Truncate a file. |
| 162 | vfs_stat | Stat a file by handle. |
| 163 | vfs_unlink | Unlink a name under a directory. |
| 164 | vfs_resolve | Resolve (open) a node by path. |
| 165 | vfs_create_path | Create a node by path. |
| 166 | vfs_unlink_path | Unlink a node by path. |
| 213 | file_map | Map a file's pages into the address space. |

### Block devices and NVMe

| Num | Name | Description |
|----|------|-------------|
| 179 | block_create | Create a block device (RAM-backed). |
| 180 | block_info | Query block device geometry and flags. |
| 181 | block_submit | Submit a block I/O request. |
| 182 | block_collect | Collect a completed block I/O request. |
| 183 | block_revoke | Revoke a block device. |
| 184 | block_service | Service a block device's queues. |
| 186 | block_interface_create | Register a userspace block capsule transport (driver domains only). Shared with POSIX `open`; see the numbering note. |
| 212 | nvme_open | Bind an NVMe PCI device as a block device (needs `CAP_RESOURCE_ADMIN`). |

### Time

| Num | Name | Description |
|----|------|-------------|
| 174 | ticks | Read the monotonic tick counter. |
| 214 | wall_clock | Read the RTC wall clock (UTC). |

---

## POSIX profile (static libc)

These numbers are used by the static POSIX libc and are only accepted for tasks
admitted to a POSIX profile. They are defined in `src/process/posix_abi.h`.

| Num | Name | Description |
|----|------|-------------|
| 186 | open | Open a file. Shared with `block_interface_create` (186), disambiguated by caller; see the numbering note. |
| 187 | close | Close a file descriptor. |
| 188 | read | Read from a file descriptor. |
| 189 | write | Write to a file descriptor. |
| 190 | lseek | Reposition a file offset. |
| 191 | dup | Duplicate a file descriptor. |
| 192 | dup2 | Duplicate onto a specific descriptor. |
| 193 | fcntl | File descriptor control (e.g. close-on-exec). |
| 194 | stat | Stat a path. |
| 195 | fstat | Stat an open descriptor. |
| 196 | mkdir | Create a directory. |
| 197 | rmdir | Remove a directory. |
| 198 | unlink | Remove a file. |
| 199 | chdir | Change the working directory. |
| 200 | getcwd | Get the working directory. |
| 201 | truncate | Truncate a path. |
| 202 | fork | Fork the current process. |
| 203 | execve | Replace the process image. |
| 204 | exit | Terminate the process. |
| 205 | waitpid | Wait for a child process. |
| 206 | getpid | Get the process id. |
| 207 | getppid | Get the parent process id. |
| 208 | brk | Adjust the program break (heap). |
| 209 | getrandom | Fill a buffer with random bytes. |
| 217 | getdents | Read directory entries from an open directory descriptor. |
| 218 | chmod | Change the permission bits of a path. |
| 219 | fchmod | Change permission bits on an open descriptor. |
| 220 | chown | Change file ownership by path. |
| 221 | umask | Set the process create mask and return the previous one. |
| 222 | link | Give one inode a second name on the same filesystem. |
| 223 | rename | Move one name within or across directories, replacing a same-kind target. |
| 224 | symlink | Give a path a second name that resolves to a target string. |
| 225 | readlink | Read the target string a symlink carries, without following it. |
| 226 | lstat | Stat a path without following a final symlink. |
| 227 | ftruncate | Resize an open file, extending it with zeroes. |
| 228 | pread | Read at an explicit offset without moving the cursor. |
| 229 | pwrite | Write at an explicit offset; O_APPEND is ignored. |
| 230 | fsync | Push a file's dirty pages through the staging commit. |
| 231 | fdatasync | Same staging path; the commit lands data and metadata together. |
