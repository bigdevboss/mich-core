# Driver vynos: moving in-kernel drivers to userspace capsules

Goal: shrink the in-kernel trusted computing base to the irreducible
bootstrap-plus-panic core (serial/SCLP write stub, IRQ and channel trap
dispatch, boot-module loader) and run every other device driver as a userspace
capsule over a frozen driver ABI. A smaller TCB is a direct FSTEC and audit win,
and boot inspection confirms the bootloader hands capsules into RAM via bootfs,
so no in-kernel block driver is needed to bootstrap.

`virtio_net` is the proven template: it already runs the whole virtio dance in
userspace and serves the kernel network stack over shared rings.

## Ordering (confirmed)

1. virtio-blk (this document's running example)
2. nvme
3. virtio-pci transport itself (last, since the others sit on it)

Freezing the driver ABI is the OUTCOME of this work, not a separate up-front
milestone: the contract is proven by porting real drivers to it, then frozen.

## Where things stand today

- `src/arch/x86_64/drivers/virtio_blk.c` (162 lines) drives virtio-blk entirely
  in the kernel using in-kernel helpers (`virtio_pci_*`, `virtqueue_*`,
  `dma_resource_*`) and hands the block layer two callbacks through
  `block_bind_transport(sector_count, flags, transport, queue, dma, issue,
  reap, issue_sg)` (see `src/block/block.h`).
- The block layer already runs deferred/async: devices carry `BLOCK_FLAG_DEFER`,
  `block_submit` calls `dev->issue`, `block_service` calls `dev->reap`,
  `block_collect` returns the result, and `block_wait_event` blocks a waiter.
  This async shape is exactly what a cross-process capsule needs, so the block
  layer barely changes; only the transport binding does.

## Target architecture (mirror of net)

The net capsule registers rings and a buffer pool with the kernel, then services
them as the driver side:

- `mich_vnic_create` -> kernel makes the shared pool + rx/tx rings.
- `mich_net_interface_create(pool, rx_ring, tx_ring, ...)` -> the kernel network
  stack attaches to those rings.
- The capsule loop drains TX (`..._driver_dequeue_tx`), delivers RX
  (`..._driver_receive`), and completes TX (`..._driver_complete_tx`).

Block is the same shape with request/completion semantics instead of frames:

- Shared objects: one data pool (sector payloads), one request ring
  (kernel -> capsule) and one completion ring (capsule -> kernel). Rings are the
  generic `mich_ring_create(capacity, descriptor_size)` objects the net path
  already uses; the descriptor records are defined in
  `src/block/block_driver_abi.h`.
- The capsule registers a block interface (kernel makes a `block` device object
  bound to the rings), then loops: dequeue a `block_driver_request`, run the
  virtio-blk I/O against its own virtqueue and DMA, and push a
  `block_driver_completion`.
- The kernel block layer gets a new transport binding that enqueues onto the
  request ring in `block_submit` and reaps the completion ring in
  `block_service`, replacing the in-kernel `issue`/`reap` callbacks. The public
  block API (`block_submit`, `block_service`, `block_collect`, `block_io`,
  `block_wait_event`) is unchanged, so blockfs and every block test keep working.

## Design decision to confirm

How should the kernel side route block I/O to the capsule?

- Option 1 (recommended): a dedicated kernel block-interface object that mirrors
  `net_interface`, bound to the request/completion rings, with a
  `block_bind_capsule_transport` that swaps the in-kernel `issue`/`reap` for
  ring enqueue/reap. Smallest, proven-by-net, incremental.
- Option 2: a generic "capsule transport" object shared by block, nvme, and
  later drivers. More reuse, more up-front design, higher risk to land in one
  step.

Recommendation: Option 1 now; generalize into Option 2 only if nvme shows the
two share enough to justify it. This keeps each step reviewable.

Decision (confirmed): Option 1 first (mirror net_interface), revisit generalizing
into Option 2 after nvme lands.

## Milestones

- M0 (contract): this document plus `src/block/block_driver_abi.h`, the shared
  request/completion/registration records. The freeze candidate, validated as
  the kernel object and capsule below consume it.
- M1 (walking skeleton): `src/user64/virtio_blk` capsule that boots, negotiates
  virtio-blk, reads capacity, and prints `Mich virtio-blk: bootstrap pass`.
  Reuses the net capsule bring-up sequence. Build target plus a smoke marker.
  Proves a userspace capsule can drive the device; no kernel routing yet.
- M2 (serve I/O): kernel block-interface object plus
  `block_bind_capsule_transport`; route `block_submit`/`block_service` through
  the rings; capsule serves reads and writes. Point the existing block tests
  (virtio-blk read/write, blockfs mount and io) at the capsule-backed device.
- M3 (retire in-kernel driver): drop `virtio_blk.c` from the default image once
  the capsule passes the same tests; keep it out of the TCB.
- M4: repeat M1-M3 for nvme.
- M5: move the virtio-pci transport to userspace, then freeze the driver ABI as
  the contract artifact.

## Notes

- No per-request heap on the hot path: the data pool is preallocated and indexed
  by offset, the rings are bounded (`BLOCK_REQUEST_MAX`).
- Fail closed at the trust boundary: the kernel validates every completion's
  `request_id`, `status`, and `transferred` against the outstanding request
  before trusting them; a capsule is not trusted to report more bytes than asked.
