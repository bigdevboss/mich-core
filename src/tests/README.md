# Mich Core tests

Tests live outside production subsystem files.

## Rules

- Test code calls public subsystem APIs.
- A test returns data to the test runner. It does not declare itself successful.
- The runner owns result formatting and final status.
- Production files do not print test success markers.
- Fixtures stay inside `src/tests`.
- Hardware profiles remain separate from pure subsystem tests.
- Generated images and logs stay under `bin`.

## Current migration

The independent x86-64 suite now contains:

- `object_test.c`
- `resource_test.c`
- `async_test.c`
- `vfs_test.c`
- `test_runner.c`

The runner receives an explicit environment with task objects, VM spaces, a task slot, and a result pointer. Test files do not read private architecture globals.

Remaining self-tests in `src/arch/x86_64/kernel/kernel.c` are legacy tests awaiting migration. Move them by subsystem instead of mechanically copying private kernel state into the test tree.

Network tests now live in separate files for foundation and Ethernet, ARP and IPv4, IPv6 and ICMPv6, TCP and UDPv6, IPv4 transports and sockets, and network interface stress. Shared packet builders and cycle helpers live in `net_test_support.c`.

Driver, supervisor, virtio, MSI, MSI-X, IRQ event, and platform MMIO tests now live in `driver_test.c` and `hardware_test.c`.

No self-test functions remain in `src/arch/x86_64/kernel/kernel.c`.
No self-test functions remain in `src/arch/x86_64/cpu/smp64.c`.

The production x86-64 image does not link test objects. The test image uses a separate `kernel_test.o`, links `src/tests`, records suite IDs and statuses in a bounded table, and finishes through QEMU `isa-debug-exit`. Serial output is diagnostic only. The external runner trusts process status 33 for success.
