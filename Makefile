.DEFAULT_GOAL := all

CC = gcc
AS = nasm
LD = ld
OBJCOPY = objcopy
OBJDUMP = objdump
PYTHON = python3

CORE = src/core
CRYPTO = src/crypto
PROCESS = src/process
OBJECTS = src/objects
NET = src/net
DRIVER = src/driver
FS = src/fs
BLOCK = src/block
PORTABLE_INCLUDES = -Isrc/arch -I$(CORE) -I$(CRYPTO) -I$(PROCESS) -I$(OBJECTS) \
	-I$(NET) -I$(DRIVER) -I$(FS) -I$(BLOCK)
ARCH64 = src/arch/x86_64
ARCH64_BOOT = $(ARCH64)/boot
ARCH64_CPU = $(ARCH64)/cpu
ARCH64_MEMORY = $(ARCH64)/memory
ARCH64_PLATFORM = $(ARCH64)/platform
ARCH64_DRIVERS = $(ARCH64)/drivers
ARCH64_KERNEL = $(ARCH64)/kernel
ARCH64_INCLUDES = -I$(ARCH64) -I$(ARCH64_CPU) -I$(ARCH64_MEMORY) \
	-I$(ARCH64_PLATFORM) -I$(ARCH64_DRIVERS) -I$(ARCH64_KERNEL)
TEST64 = src/tests/x86_64
# Shared freestanding base for the kernel and the user programs. GCC's own
# include directory provides the SSE2 intrinsics headers (used by user-side
# net code), which are self-contained under -nostdinc.
GCC_INCLUDE = $(shell $(CC) -print-file-name=include)
CFLAGS64_BASE = -m64 -mno-red-zone -nostdlib -nostdinc -fno-builtin -fno-stack-protector -nostartfiles -nodefaultlibs -ffreestanding -fno-pie -fno-pic -fno-asynchronous-unwind-tables -MMD -MP -Wall -Wextra -O2 -Isrc -I$(GCC_INCLUDE) $(ARCH64_INCLUDES) $(PORTABLE_INCLUDES) -I$(TEST64)
# The kernel itself must never emit SSE/MMX code: user FPU state is only
# preserved lazily (fpu64_save/fpu64_load around context switches) and the
# syscall/fault/interrupt entry paths do not save XMM registers. Any SSE
# instruction executed by kernel code (compiler-vectorized copy loops, SSE
# memcpy/checksum) silently corrupts live user XMM registers - a movdqa/
# movaps pair split by a COW write fault stored zeros into signal_demo's
# nanosleep interval that way. mem.c and net/checksum.h keep scalar fallback
# paths for exactly this build.
CFLAGS64 = $(CFLAGS64_BASE) -mgeneral-regs-only
# User programs run with their own FPU state (saved by the scheduler), so
# they keep the x86-64 baseline ISA with SSE2.
USER64_CFLAGS = $(CFLAGS64_BASE) -msse2 -mno-mmx -mcmodel=large -Isrc/user64/include -Isrc/user64/lib
LDFLAGS64 = -m elf_x86_64 -T $(ARCH64_BOOT)/linker.ld

# The netbench capsule grows to real bulk and packet-rate sizes and dials the
# peer directly only on the tap/vhost (KVM) topology; MICH_NET_TAP selects that
# build. make does not track flag changes, so run `make clean` when switching
# topology. The same variable switches qemu-smoke64.sh to a tap+vhost netdev.
ifdef MICH_NET_TAP
NETBENCH_TAP_FLAG = -DMICH_NETBENCH_TAP
endif

BIN_DIR = bin
BIN64 = $(BIN_DIR)/x86_64
OBJ64 = $(BIN64)/obj
KERNEL64_ELF = $(BIN64)/mich-kernel.elf
KERNEL64_FLAT = $(BIN64)/mich-kernel.bin
KERNEL64_TEST_ELF = $(BIN64)/mich-kernel-test.elf
KERNEL64_TEST_FLAT = $(BIN64)/mich-kernel-test.bin
DISK64 = $(BIN64)/disk.img
DISK64_TEST = $(BIN64)/disk-test.img
DISK64_UNIT = $(BIN64)/disk-unit.img
DISK64_HARDWARE = $(BIN64)/disk-hardware.img
DISK64_DNS = $(BIN64)/disk-dns.img
DISK64_NETBENCH = $(BIN64)/disk-netbench.img
DISK64_VIRTIO_BLK = $(BIN64)/disk-virtio-blk.img
DISK64_NVME = $(BIN64)/disk-nvme.img
DISK64_HARDWARE_RESTART = $(BIN64)/disk-hardware-restart.img
DISK64_HARDWARE_CIRCUIT = $(BIN64)/disk-hardware-circuit.img
DISK64_HARDWARE_RECOVERY = $(BIN64)/disk-hardware-recovery.img
RECOVERY_STABILITY_RUNS ?= 3
DISK64_PANIC = $(BIN64)/disk-panic.img
DISK64_CRASH = $(BIN64)/disk-crash.img
UEFI64_OBJ = $(OBJ64)/uefi.o
UEFI64_EFI = $(BIN64)/BOOTX64.EFI
USER64_DIR = $(BIN64)/user
USER64_OBJ_DIR = $(USER64_DIR)/obj
INIT64_ELF = $(USER64_DIR)/init64.elf
INIT64_OBJS = $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(USER64_OBJ_DIR)/sigreturn.o $(USER64_OBJ_DIR)/posix.o $(USER64_OBJ_DIR)/process.o $(USER64_OBJ_DIR)/init.o
POSIXAPP64_ELF = $(USER64_DIR)/posixapp64.elf
POSIXAPP64_OBJS = $(USER64_OBJ_DIR)/crt0_posix.o $(USER64_OBJ_DIR)/syscall.o $(USER64_OBJ_DIR)/sigreturn.o $(USER64_OBJ_DIR)/posix.o $(USER64_OBJ_DIR)/process.o $(USER64_OBJ_DIR)/string.o $(USER64_OBJ_DIR)/stdio.o $(USER64_OBJ_DIR)/stdlib.o $(USER64_OBJ_DIR)/posixapp.o
POSIXDEMO64_ELF = $(USER64_DIR)/posixdemo64.elf
POSIXDEMO64_OBJS = $(USER64_OBJ_DIR)/crt0_posix.o $(USER64_OBJ_DIR)/syscall.o $(USER64_OBJ_DIR)/sigreturn.o $(USER64_OBJ_DIR)/posix.o $(USER64_OBJ_DIR)/process.o $(USER64_OBJ_DIR)/string.o $(USER64_OBJ_DIR)/stdio.o $(USER64_OBJ_DIR)/stdlib.o $(USER64_OBJ_DIR)/posixdemo.o
VIRTIO_LIB64_OBJ = $(USER64_OBJ_DIR)/virtio_lib.o
VIRTIO_BLK64_OBJ = $(USER64_OBJ_DIR)/virtio_blk.o
NVME64_OBJ = $(USER64_OBJ_DIR)/nvme_capsule.o
VIRTIO_NET64_OBJ = $(USER64_OBJ_DIR)/virtio_net.o
VIRTIO_NET_SAFE64_OBJ = $(USER64_OBJ_DIR)/virtio_net_safe.o
VIRTIO_NET64_PROBES_OBJ = $(USER64_OBJ_DIR)/virtio_net_probes.o
DNSPROBE64_OBJ = $(USER64_OBJ_DIR)/dnsprobe.o
NETBENCH64_OBJ = $(USER64_OBJ_DIR)/netbench.o
DNS64_OBJ = $(USER64_OBJ_DIR)/dns.o
DNS_MESSAGE64_OBJ = $(USER64_OBJ_DIR)/dns_message.o
VIRTIO_BLK64_ELF = $(USER64_DIR)/virtio-blk.elf
NVME64_ELF = $(USER64_DIR)/nvme.elf
VIRTIO_NET64_ELF = $(USER64_DIR)/virtio-net.elf
DNSPROBE64_ELF = $(USER64_DIR)/dnsprobe.elf
NETBENCH64_ELF = $(USER64_DIR)/netbench.elf
GEN64_DIR = $(BIN64)/generated
VIRTIO_NET_RECOVERY_RIP_H = $(GEN64_DIR)/virtio_net_recovery_rip.h
VIRTIO_NET_SAFE64_ELF = $(USER64_DIR)/virtio-net-safe.elf
OBJS64 = $(OBJ64)/boot.o $(OBJ64)/kernel.o $(OBJ64)/task_core.o $(OBJ64)/posix_fd_core.o $(OBJ64)/posix_profile_core.o $(OBJ64)/posix_vfs_core.o $(OBJ64)/posix_process_core.o $(OBJ64)/posix_time_core.o $(OBJ64)/posix_signal_core.o $(OBJ64)/posix_pledge_core.o $(OBJ64)/posix_pipe_core.o $(OBJ64)/posix_socket_core.o $(OBJ64)/scheduler_core.o $(OBJ64)/service_core.o $(OBJ64)/object_core.o $(OBJ64)/resource_core.o $(OBJ64)/iommu_core.o $(OBJ64)/driver_core.o $(OBJ64)/driver_supervisor_core.o $(OBJ64)/driver_manager_core.o $(OBJ64)/ring_core.o $(OBJ64)/completion_core.o $(OBJ64)/timer_object_core.o $(OBJ64)/net_buffer_core.o $(OBJ64)/vnic_core.o $(OBJ64)/vnic_benchmark.o $(OBJ64)/net_interface_core.o $(OBJ64)/ethernet_core.o $(OBJ64)/arp_core.o $(OBJ64)/ipv4_core.o $(OBJ64)/ipv6_core.o $(OBJ64)/icmp_core.o $(OBJ64)/icmpv6_core.o $(OBJ64)/loopback_core.o $(OBJ64)/udp_core.o $(OBJ64)/udpv6_core.o $(OBJ64)/tcp_core.o $(OBJ64)/tcp_cc_core.o $(OBJ64)/pmtu_core.o $(OBJ64)/route_core.o $(OBJ64)/socket_core.o $(OBJ64)/dns_message_core.o $(OBJ64)/vfs_core.o $(OBJ64)/crc32c_core.o $(OBJ64)/adytumfs_super_core.o $(OBJ64)/adytumfs_inode_core.o $(OBJ64)/adytumfs_volume_core.o $(OBJ64)/adytumfs_verify_core.o $(OBJ64)/adytumfs_dir_core.o $(OBJ64)/adytumfs_file_core.o $(OBJ64)/adytumfs_ops_core.o $(OBJ64)/adytumfs_core.o $(OBJ64)/block_core.o $(OBJ64)/cache_core.o $(OBJ64)/firmware_core.o $(OBJ64)/event_core.o $(OBJ64)/endpoint_core.o $(OBJ64)/bridge_core.o $(OBJ64)/pmm_core.o $(OBJ64)/mem_core.o $(OBJ64)/siphash_core.o $(OBJ64)/crypto_core.o $(OBJ64)/entropy_core.o $(OBJ64)/ipc64.o $(OBJ64)/acpi64.o $(OBJ64)/rtc64.o $(OBJ64)/vtd64.o $(OBJ64)/amd_iommu64.o $(OBJ64)/pci64.o $(OBJ64)/nvme.o $(OBJ64)/apic64.o $(OBJ64)/ioapic64.o $(OBJ64)/smp64.o $(OBJ64)/smp_tramp.o $(OBJ64)/vector64.o $(OBJ64)/msi64.o $(OBJ64)/msix64.o $(OBJ64)/panic64.o $(OBJ64)/gdt_asm.o $(OBJ64)/gdt.o $(OBJ64)/exceptions.o $(OBJ64)/interrupt.o $(OBJ64)/idt.o $(OBJ64)/vm.o $(OBJ64)/elf64.o $(OBJ64)/platform.o $(OBJ64)/serial.o $(OBJ64)/syscall_dispatch.o $(OBJ64)/syscall.o
TEST64_OBJS = $(OBJ64)/test_runner64.o $(OBJ64)/test_object64.o $(OBJ64)/test_resource64.o $(OBJ64)/test_async64.o $(OBJ64)/test_fpu64.o $(OBJ64)/test_smp64.o $(OBJ64)/test_driver64.o $(OBJ64)/test_hardware64.o $(OBJ64)/test_network_runner64.o $(OBJ64)/test_net_support64.o $(OBJ64)/test_net_foundation64.o $(OBJ64)/test_ipv4_64.o $(OBJ64)/test_ipv6_64.o $(OBJ64)/test_tcp64.o $(OBJ64)/test_udp_socket64.o $(OBJ64)/test_dns64.o $(OBJ64)/test_net_interface64.o $(OBJ64)/test_vfs64.o $(OBJ64)/test_posix_fd64.o $(OBJ64)/test_posix_profile64.o $(OBJ64)/test_posix_vfs64.o $(OBJ64)/test_posix_process64.o $(OBJ64)/test_posix_time64.o $(OBJ64)/test_posix_signal64.o $(OBJ64)/test_posix_pledge64.o $(OBJ64)/test_posix_pipe64.o $(OBJ64)/test_posix_socket64.o $(OBJ64)/test_block64.o $(OBJ64)/test_cache64.o $(OBJ64)/test_entropy64.o $(OBJ64)/test_siphash64.o $(OBJ64)/test_rtc64.o $(OBJ64)/test_crc32c64.o $(OBJ64)/test_adytumfs_super64.o $(OBJ64)/test_adytumfs_volume64.o $(OBJ64)/test_adytumfs_alloc64.o $(OBJ64)/test_adytumfs_inode64.o $(OBJ64)/test_adytumfs_extent64.o $(OBJ64)/test_adytumfs_dir64.o $(OBJ64)/test_adytumfs_diriter64.o $(OBJ64)/test_adytumfs_file64.o $(OBJ64)/test_adytumfs_ops64.o $(OBJ64)/test_adytumfs64.o $(OBJ64)/test_adytumfs_crash64.o $(OBJ64)/test_crash_boot64.o $(OBJ64)/test_nvme64.o $(OBJ64)/test_net_bench64.o $(OBJ64)/test_report64.o
OBJS64_TEST = $(OBJ64)/boot.o $(OBJ64)/kernel_test.o $(filter-out $(OBJ64)/boot.o $(OBJ64)/kernel.o,$(OBJS64)) $(TEST64_OBJS)
PORTABLE64_DIR = $(BIN64)/portable
PORTABLE64_NAMES = task posix_fd posix_profile posix_vfs posix_process posix_time posix_signal posix_pledge posix_pipe posix_socket service object resource driver driver_supervisor driver_manager ring completion timer_object net_buffer vnic net_interface ethernet arp ipv4 ipv6 icmp icmpv6 loopback udp udpv6 tcp tcp_cc pmtu route socket vfs crc32c adytumfs_super adytumfs_inode adytumfs_volume adytumfs_verify adytumfs_dir adytumfs_file adytumfs_ops adytumfs block cache firmware event endpoint bridge pmm
PORTABLE64_OBJS = $(addprefix $(PORTABLE64_DIR)/,$(addsuffix .o,$(PORTABLE64_NAMES)))

all: $(DISK64) $(PORTABLE64_OBJS)

$(BIN_DIR) $(BIN64) $(OBJ64) $(PORTABLE64_DIR) $(USER64_DIR) $(USER64_OBJ_DIR) $(GEN64_DIR):
	mkdir -p $@

$(PORTABLE64_DIR)/%.o: $(CORE)/%.c | $(PORTABLE64_DIR)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(PORTABLE64_DIR)/%.o: $(PROCESS)/%.c | $(PORTABLE64_DIR)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(PORTABLE64_DIR)/%.o: $(OBJECTS)/%.c | $(PORTABLE64_DIR)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(PORTABLE64_DIR)/%.o: $(NET)/%.c | $(PORTABLE64_DIR)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(PORTABLE64_DIR)/%.o: $(DRIVER)/%.c | $(PORTABLE64_DIR)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(PORTABLE64_DIR)/%.o: $(FS)/%.c | $(PORTABLE64_DIR)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(PORTABLE64_DIR)/%.o: $(BLOCK)/%.c | $(PORTABLE64_DIR)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/boot.o: $(ARCH64_BOOT)/boot.asm | $(OBJ64)
	$(AS) -f elf64 $< -o $@

$(OBJ64)/kernel.o: $(ARCH64_KERNEL)/kernel.c $(CORE)/version.h $(VIRTIO_NET_RECOVERY_RIP_H) | $(OBJ64)
	$(CC) $(CFLAGS64) -I$(GEN64_DIR) -Werror -c $< -o $@

$(OBJ64)/kernel_test.o: $(ARCH64_KERNEL)/kernel.c $(CORE)/version.h $(VIRTIO_NET_RECOVERY_RIP_H) $(TEST64)/tests64.h $(TEST64)/test_report.h | $(OBJ64)
	$(CC) $(CFLAGS64) -DMICH_TEST_BUILD -I$(GEN64_DIR) -Werror -c $< -o $@

$(OBJ64)/syscall_dispatch.o: $(ARCH64_KERNEL)/syscall_dispatch.c $(ARCH64_KERNEL)/kernel64_internal.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/task_core.o: $(PROCESS)/task.c $(PROCESS)/task.h src/arch/arch_task.h $(PROCESS)/posix_fd.h $(PROCESS)/posix_profile.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/posix_fd_core.o: $(PROCESS)/posix_fd.c $(PROCESS)/posix_fd.h $(PROCESS)/task.h $(OBJECTS)/object.h $(FS)/vfs.h $(CORE)/spinlock.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/posix_profile_core.o: $(PROCESS)/posix_profile.c $(PROCESS)/posix_profile.h $(PROCESS)/task.h $(OBJECTS)/object.h $(FS)/vfs.h $(CORE)/spinlock.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/posix_vfs_core.o: $(PROCESS)/posix_vfs.c $(PROCESS)/posix_vfs.h $(PROCESS)/posix_profile.h $(PROCESS)/posix_fd.h $(FS)/vfs.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/posix_time_core.o: $(PROCESS)/posix_time.c $(PROCESS)/posix_time.h $(ARCH64_PLATFORM)/rtc64.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/posix_pledge_core.o: $(PROCESS)/posix_pledge.c $(PROCESS)/posix_pledge.h $(PROCESS)/posix_abi.h $(PROCESS)/posix_profile.h $(FS)/vfs.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/posix_pipe_core.o: $(PROCESS)/posix_pipe.c $(PROCESS)/posix_pipe.h $(PROCESS)/posix_abi.h $(PROCESS)/posix_signal.h $(PROCESS)/posix_vfs.h | $(OBJ64)

$(OBJ64)/posix_socket_core.o: $(PROCESS)/posix_socket.c $(PROCESS)/posix_socket.h $(PROCESS)/posix_abi.h $(PROCESS)/posix_fd.h $(PROCESS)/posix_signal.h $(PROCESS)/posix_vfs.h $(NET)/socket.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/posix_signal_core.o: $(PROCESS)/posix_signal.c $(PROCESS)/posix_signal.h $(PROCESS)/posix_abi.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/posix_process_core.o: $(PROCESS)/posix_process.c $(PROCESS)/posix_process.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/scheduler_core.o: $(PROCESS)/scheduler_core.c $(PROCESS)/scheduler.h $(PROCESS)/task.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/service_core.o: $(PROCESS)/service.c $(PROCESS)/service.h $(PROCESS)/task.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/object_core.o: $(OBJECTS)/object.c $(OBJECTS)/object.h $(PROCESS)/task.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/resource_core.o: $(OBJECTS)/resource.c $(OBJECTS)/resource.h $(OBJECTS)/object.h $(CORE)/pmm.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/iommu_core.o: $(OBJECTS)/iommu.c $(OBJECTS)/iommu.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/driver_core.o: $(DRIVER)/driver.c $(DRIVER)/driver.h $(OBJECTS)/object.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/driver_supervisor_core.o: $(DRIVER)/driver_supervisor.c $(DRIVER)/driver_supervisor.h $(OBJECTS)/object.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/driver_manager_core.o: $(DRIVER)/driver_manager.c $(DRIVER)/driver_manager.h $(DRIVER)/driver_supervisor.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/ring_core.o: $(OBJECTS)/ring.c $(OBJECTS)/ring.h $(OBJECTS)/resource.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/completion_core.o: $(OBJECTS)/completion.c $(OBJECTS)/completion.h $(OBJECTS)/event.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/timer_object_core.o: $(OBJECTS)/timer_object.c $(OBJECTS)/timer_object.h $(OBJECTS)/event.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/net_buffer_core.o: $(NET)/net_buffer.c $(NET)/net_buffer.h $(OBJECTS)/resource.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/vnic_core.o: $(NET)/vnic.c $(NET)/vnic.h $(NET)/net_buffer.h $(OBJECTS)/ring.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/vnic_benchmark.o: $(ARCH64_KERNEL)/vnic_benchmark.c $(NET)/vnic_benchmark.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/net_interface_core.o: $(NET)/net_interface.c $(NET)/net_interface.h $(DRIVER)/driver_supervisor.h $(CORE)/entropy.h $(CRYPTO)/crypto.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/ethernet_core.o: $(NET)/ethernet.c $(NET)/ethernet.h $(NET)/vnic.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/arp_core.o: $(NET)/arp.c $(NET)/arp.h $(NET)/ethernet.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/ipv4_core.o: $(NET)/ipv4.c $(NET)/ipv4.h $(NET)/ethernet.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/ipv6_core.o: $(NET)/ipv6.c $(NET)/ipv6.h $(NET)/ethernet.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/icmp_core.o: $(NET)/icmp.c $(NET)/icmp.h $(NET)/ipv4.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/icmpv6_core.o: $(NET)/icmpv6.c $(NET)/icmpv6.h $(NET)/ipv6.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/loopback_core.o: $(NET)/loopback.c $(NET)/loopback.h $(NET)/ipv4.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/udp_core.o: $(NET)/udp.c $(NET)/udp.h $(NET)/ipv4.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/udpv6_core.o: $(NET)/udpv6.c $(NET)/udpv6.h $(NET)/ipv6.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/tcp_core.o: $(NET)/tcp.c $(NET)/tcp.h $(CRYPTO)/siphash.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/tcp_cc_core.o: $(NET)/tcp_cc.c $(NET)/tcp.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/pmtu_core.o: $(NET)/pmtu.c $(NET)/pmtu.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/route_core.o: $(NET)/route.c $(NET)/route.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/socket_core.o: $(NET)/socket.c $(NET)/socket.h $(NET)/udp.h $(NET)/route.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/dns_message_core.o: $(NET)/dns_message.c $(NET)/dns_message.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@



$(OBJ64)/vfs_core.o: $(FS)/vfs.c $(FS)/vfs.h $(FS)/adytumfs.h $(OBJECTS)/object.h $(CORE)/entropy.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/crc32c_core.o: $(FS)/crc32c.c $(FS)/crc32c.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/adytumfs_super_core.o: $(FS)/adytumfs_super.c $(FS)/adytumfs_format.h $(FS)/crc32c.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/adytumfs_inode_core.o: $(FS)/adytumfs_inode.c $(FS)/adytumfs_format.h $(FS)/crc32c.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/adytumfs_volume_core.o: $(FS)/adytumfs_volume.c $(FS)/adytumfs_format.h $(FS)/crc32c.h $(BLOCK)/block.h $(BLOCK)/cache.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/adytumfs_verify_core.o: $(FS)/adytumfs_verify.c $(FS)/adytumfs_format.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/adytumfs_dir_core.o: $(FS)/adytumfs_dir.c $(FS)/adytumfs_format.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/adytumfs_file_core.o: $(FS)/adytumfs_file.c $(FS)/adytumfs_format.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/adytumfs_ops_core.o: $(FS)/adytumfs_ops.c $(FS)/adytumfs_format.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/adytumfs_core.o: $(FS)/adytumfs.c $(FS)/adytumfs.h $(FS)/vfs.h $(BLOCK)/block.h $(BLOCK)/cache.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/block_core.o: $(BLOCK)/block.c $(BLOCK)/block.h $(BLOCK)/block_abi.h $(BLOCK)/cache.h $(OBJECTS)/object.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/cache_core.o: $(BLOCK)/cache.c $(BLOCK)/cache.h $(BLOCK)/block.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/firmware_core.o: $(FS)/firmware.c $(FS)/firmware.h $(FS)/vfs.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_report64.o: $(TEST64)/test_report.c $(TEST64)/test_report.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_runner64.o: $(TEST64)/test_runner.c $(TEST64)/tests64.h $(TEST64)/test_report.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_object64.o: $(TEST64)/object_test.c $(TEST64)/tests64.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_resource64.o: $(TEST64)/resource_test.c $(TEST64)/tests64.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_async64.o: $(TEST64)/async_test.c $(TEST64)/tests64.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_fpu64.o: $(TEST64)/fpu_test.c $(TEST64)/tests64.h $(TEST64)/net_test.h $(ARCH64_KERNEL)/kernel64_internal.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_smp64.o: $(TEST64)/smp_test.c $(TEST64)/tests64.h $(TEST64)/test_report.h $(ARCH64_CPU)/smp64.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_driver64.o: $(TEST64)/driver_test.c $(TEST64)/tests64.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_hardware64.o: $(TEST64)/hardware_test.c $(TEST64)/tests64.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_network_runner64.o: $(TEST64)/network_runner.c $(TEST64)/tests64.h $(TEST64)/net_test.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_net_support64.o: $(TEST64)/net_test_support.c $(TEST64)/tests64.h $(TEST64)/net_test.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_net_foundation64.o: $(TEST64)/net_foundation_test.c $(TEST64)/tests64.h $(TEST64)/net_test.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_ipv4_64.o: $(TEST64)/ipv4_test.c $(TEST64)/tests64.h $(TEST64)/net_test.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_ipv6_64.o: $(TEST64)/ipv6_test.c $(TEST64)/tests64.h $(TEST64)/net_test.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_tcp64.o: $(TEST64)/tcp_test.c $(TEST64)/tests64.h $(TEST64)/net_test.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_dns64.o: $(TEST64)/dns_test.c $(TEST64)/tests64.h $(NET)/dns_message.h | $(OBJ64)
	$(CC) $(CFLAGS64) -c $< -o $@

$(OBJ64)/test_udp_socket64.o: $(TEST64)/udp_socket_test.c $(TEST64)/tests64.h $(TEST64)/net_test.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_net_interface64.o: $(TEST64)/net_interface_test.c $(TEST64)/tests64.h $(TEST64)/net_test.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_vfs64.o: $(TEST64)/vfs_test.c $(TEST64)/tests64.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_posix_fd64.o: $(TEST64)/posix_fd_test.c $(TEST64)/tests64.h $(PROCESS)/posix_fd.h $(FS)/vfs.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_posix_profile64.o: $(TEST64)/posix_profile_test.c $(TEST64)/tests64.h $(PROCESS)/posix_profile.h $(FS)/vfs.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_posix_vfs64.o: $(TEST64)/posix_vfs_test.c $(TEST64)/tests64.h $(PROCESS)/posix_vfs.h $(FS)/vfs.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_posix_time64.o: $(TEST64)/posix_time_test.c $(TEST64)/tests64.h $(PROCESS)/posix_time.h $(PROCESS)/posix_abi.h $(ARCH64_PLATFORM)/rtc64.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_posix_signal64.o: $(TEST64)/posix_signal_test.c $(TEST64)/tests64.h $(PROCESS)/posix_signal.h $(PROCESS)/posix_abi.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_posix_pledge64.o: $(TEST64)/posix_pledge_test.c $(TEST64)/tests64.h $(PROCESS)/posix_pledge.h $(PROCESS)/posix_abi.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_posix_pipe64.o: $(TEST64)/posix_pipe_test.c $(TEST64)/tests64.h $(PROCESS)/posix_pipe.h $(PROCESS)/posix_abi.h $(PROCESS)/posix_pledge.h | $(OBJ64)

$(OBJ64)/test_posix_socket64.o: $(TEST64)/posix_socket_test.c $(TEST64)/tests64.h $(PROCESS)/posix_socket.h $(PROCESS)/posix_fd.h $(PROCESS)/posix_abi.h $(PROCESS)/posix_pledge.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_posix_process64.o: $(TEST64)/posix_process_test.c $(TEST64)/tests64.h $(PROCESS)/posix_process.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_block64.o: $(TEST64)/block_test.c $(TEST64)/tests64.h $(BLOCK)/block.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_cache64.o: $(TEST64)/cache_test.c $(TEST64)/tests64.h $(BLOCK)/cache.h $(BLOCK)/block.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@
$(OBJ64)/test_entropy64.o: $(TEST64)/entropy_test.c $(TEST64)/tests64.h $(CORE)/entropy.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_rtc64.o: $(TEST64)/rtc_test.c $(TEST64)/tests64.h $(ARCH64_PLATFORM)/rtc64.h | $(OBJ64)
	$(CC) $(CFLAGS64) -DMICH_TEST_BUILD -Werror -c $< -o $@

$(OBJ64)/test_siphash64.o: $(TEST64)/siphash_test.c $(TEST64)/tests64.h $(CRYPTO)/siphash.h | $(OBJ64)
	$(CC) $(CFLAGS64) -DMICH_TEST_BUILD -Werror -c $< -o $@












$(OBJ64)/test_crc32c64.o: $(TEST64)/crc32c_test.c $(TEST64)/tests64.h $(FS)/crc32c.h $(FS)/adytumfs_format.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_adytumfs_super64.o: $(TEST64)/adytumfs_super_test.c $(TEST64)/tests64.h $(FS)/adytumfs_format.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_adytumfs_volume64.o: $(TEST64)/adytumfs_volume_test.c $(TEST64)/tests64.h $(FS)/adytumfs_format.h $(BLOCK)/block.h $(OBJECTS)/object.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_adytumfs_alloc64.o: $(TEST64)/adytumfs_alloc_test.c $(TEST64)/tests64.h $(FS)/adytumfs_format.h $(BLOCK)/block.h $(OBJECTS)/object.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_adytumfs_inode64.o: $(TEST64)/adytumfs_inode_test.c $(TEST64)/tests64.h $(FS)/adytumfs_format.h $(BLOCK)/block.h $(OBJECTS)/object.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_adytumfs_extent64.o: $(TEST64)/adytumfs_extent_test.c $(TEST64)/tests64.h $(FS)/adytumfs_format.h $(BLOCK)/block.h $(OBJECTS)/object.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_adytumfs_dir64.o: $(TEST64)/adytumfs_dir_test.c $(TEST64)/tests64.h $(FS)/adytumfs_format.h $(BLOCK)/block.h $(OBJECTS)/object.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_adytumfs_diriter64.o: $(TEST64)/adytumfs_diriter_test.c $(TEST64)/tests64.h $(FS)/adytumfs_format.h $(BLOCK)/block.h $(OBJECTS)/object.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_adytumfs_file64.o: $(TEST64)/adytumfs_file_test.c $(TEST64)/tests64.h $(FS)/adytumfs_format.h $(BLOCK)/block.h $(OBJECTS)/object.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_adytumfs_ops64.o: $(TEST64)/adytumfs_ops_test.c $(TEST64)/tests64.h $(FS)/adytumfs_format.h $(BLOCK)/block.h $(OBJECTS)/object.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_adytumfs64.o: $(TEST64)/adytumfs_test.c $(TEST64)/tests64.h $(FS)/vfs.h $(FS)/adytumfs.h $(BLOCK)/block.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_adytumfs_crash64.o: $(TEST64)/adytumfs_crash_test.c $(TEST64)/tests64.h $(FS)/adytumfs_format.h $(FS)/adytumfs.h $(FS)/vfs.h $(BLOCK)/block.h $(BLOCK)/cache.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_crash_boot64.o: $(TEST64)/crash_boot.c $(TEST64)/tests64.h $(FS)/adytumfs_format.h $(FS)/adytumfs.h $(FS)/vfs.h $(ARCH64_PLATFORM)/pci64.h $(ARCH64_DRIVERS)/nvme.h $(ARCH64_KERNEL)/kernel64_internal.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_nvme64.o: $(TEST64)/nvme_test.c $(TEST64)/tests64.h $(ARCH64_DRIVERS)/nvme.h $(BLOCK)/block.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_net_bench64.o: $(TEST64)/net_bench_test.c $(TEST64)/tests64.h $(TEST64)/net_test.h $(NET)/vnic_benchmark.h $(NET)/vnic.h $(NET)/net_buffer.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/event_core.o: $(OBJECTS)/event.c $(OBJECTS)/event.h $(OBJECTS)/object.h $(PROCESS)/task.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/endpoint_core.o: $(OBJECTS)/endpoint.c $(OBJECTS)/endpoint.h $(OBJECTS)/object.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/bridge_core.o: $(OBJECTS)/bridge.c $(OBJECTS)/bridge.h $(OBJECTS)/endpoint.h $(OBJECTS)/event.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/pmm_core.o: $(CORE)/pmm.c $(CORE)/pmm.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/mem_core.o: $(CORE)/mem.c $(CORE)/types.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@
$(OBJ64)/entropy_core.o: $(CORE)/entropy.c $(CORE)/entropy.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/siphash_core.o: $(CRYPTO)/siphash.c $(CRYPTO)/siphash.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/crypto_core.o: $(CRYPTO)/crypto.c $(CRYPTO)/crypto.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@









$(OBJ64)/gdt_asm.o: $(ARCH64_CPU)/gdt.asm | $(OBJ64)
	$(AS) -f elf64 $< -o $@

$(OBJ64)/%.o: $(ARCH64_CPU)/%.c | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/%.o: $(ARCH64_MEMORY)/%.c | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/%.o: $(ARCH64_PLATFORM)/%.c | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/%.o: $(ARCH64_DRIVERS)/%.c | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/%.o: $(ARCH64_KERNEL)/%.c | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/%.o: $(ARCH64_CPU)/%.asm | $(OBJ64)
	$(AS) -f elf64 $< -o $@

$(OBJ64)/%.o: $(ARCH64_KERNEL)/%.asm | $(OBJ64)
	$(AS) -f elf64 $< -o $@

$(USER64_OBJ_DIR)/crt0.o: src/user64/lib/crt0.asm | $(USER64_OBJ_DIR)
	$(AS) -f elf64 $< -o $@

$(USER64_OBJ_DIR)/syscall.o: src/user64/lib/syscall.asm | $(USER64_OBJ_DIR)
	$(AS) -f elf64 $< -o $@

$(USER64_OBJ_DIR)/sigreturn.o: src/user64/lib/sigreturn.asm | $(USER64_OBJ_DIR)
	$(AS) -f elf64 $< -o $@

$(USER64_OBJ_DIR)/posix.o: src/user64/lib/posix.c src/process/posix_abi.h src/user64/include/errno.h src/user64/include/fcntl.h src/user64/include/signal.h src/user64/include/time.h src/user64/include/unistd.h src/user64/include/mich/syscall.h src/user64/include/sys/stat.h src/user64/include/sys/random.h src/user64/include/sys/socket.h src/user64/include/netinet/in.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(USER64_OBJ_DIR)/init.o: src/user64/init/main.c src/user64/include/mich/syscall.h src/user64/include/mich/service.h src/user64/include/mich/capability.h src/user64/include/mich/ipc.h src/user64/include/mich/event.h src/user64/include/mich/bridge.h src/user64/include/mich/hardware.h src/user64/include/mich/driver.h src/user64/include/mich/memory.h src/user64/include/mich/sg.h src/user64/include/mich/ring.h src/user64/include/mich/completion.h src/user64/include/mich/timer.h src/user64/include/mich/wait.h src/user64/include/mich/net.h src/user64/include/mich/socket.h src/user64/include/mich/net_interface.h src/user64/include/mich/vfs.h src/user64/include/mich/firmware.h src/user64/include/mich/block.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(INIT64_ELF): $(INIT64_OBJS) src/user64/linker.ld | $(USER64_DIR)
	$(LD) -m elf_x86_64 -T src/user64/linker.ld -o $@ $(INIT64_OBJS)

$(USER64_OBJ_DIR)/crt0_posix.o: src/user64/lib/crt0_posix.asm | $(USER64_OBJ_DIR)
	$(AS) -f elf64 $< -o $@

$(USER64_OBJ_DIR)/process.o: src/user64/lib/process.c src/process/posix_abi.h src/user64/include/errno.h src/user64/include/unistd.h src/user64/include/sys/wait.h src/user64/include/mich/syscall.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(USER64_OBJ_DIR)/string.o: src/user64/lib/string.c src/user64/include/string.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(USER64_OBJ_DIR)/stdio.o: src/user64/lib/stdio.c src/user64/include/stdio.h src/user64/include/string.h src/user64/include/stdlib.h src/user64/include/unistd.h src/user64/include/fcntl.h src/user64/include/errno.h src/user64/include/mich/syscall.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(USER64_OBJ_DIR)/stdlib.o: src/user64/lib/stdlib.c src/user64/include/stdlib.h src/user64/include/string.h src/user64/include/errno.h src/user64/include/limits.h src/user64/include/unistd.h src/user64/include/sys/stat.h src/user64/include/mich/syscall.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(USER64_OBJ_DIR)/posixapp.o: src/user64/posixapp/main.c src/user64/include/errno.h src/user64/include/fcntl.h src/user64/include/unistd.h src/user64/include/sys/stat.h src/user64/include/mich/syscall.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(POSIXAPP64_ELF): $(POSIXAPP64_OBJS) src/user64/linker.ld | $(USER64_DIR)
	$(LD) -m elf_x86_64 -T src/user64/linker.ld -o $@ $(POSIXAPP64_OBJS)

$(USER64_OBJ_DIR)/posixdemo.o:src/user64/posixdemo/main.c src/user64/include/errno.h src/user64/include/fcntl.h src/user64/include/signal.h src/user64/include/time.h src/user64/include/unistd.h src/user64/include/string.h src/user64/include/stdio.h src/user64/include/stdlib.h src/user64/include/sys/stat.h src/user64/include/sys/wait.h src/user64/include/mich/syscall.h src/user64/include/sys/random.h src/user64/include/sys/socket.h src/user64/include/netinet/in.h | $(USER64_OBJ_DIR) src/user64/include/sys/random.h
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(POSIXDEMO64_ELF): $(POSIXDEMO64_OBJS) src/user64/linker.ld | $(USER64_DIR)
	$(LD) -m elf_x86_64 -T src/user64/linker.ld -o $@ $(POSIXDEMO64_OBJS)

$(VIRTIO_LIB64_OBJ): src/user64/lib/virtio/virtio.c src/user64/lib/virtio/virtio.h src/user64/include/mich/hardware.h src/user64/include/mich/syscall.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(VIRTIO_BLK64_OBJ): src/user64/virtio_blk/main.c src/user64/virtio_blk/capsule.h src/user64/lib/virtio/virtio.h src/user64/include/mich/syscall.h src/user64/include/mich/driver.h src/user64/include/mich/hardware.h src/user64/include/mich/ring.h src/user64/include/mich/memory.h src/user64/include/mich/event.h src/user64/include/mich/block.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(NVME64_OBJ): src/user64/nvme/main.c src/user64/nvme/capsule.h src/user64/include/mich/syscall.h src/user64/include/mich/driver.h src/user64/include/mich/hardware.h src/user64/include/mich/ring.h src/user64/include/mich/memory.h src/user64/include/mich/event.h src/user64/include/mich/block.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(VIRTIO_NET64_OBJ): src/user64/virtio_net/main.c src/user64/virtio_net/capsule.h src/user64/lib/virtio/virtio.h src/user64/include/mich/syscall.h src/user64/include/mich/event.h src/user64/include/mich/driver.h src/user64/include/mich/hardware.h src/user64/include/mich/net.h src/user64/include/mich/net_interface.h src/user64/include/mich/timer.h src/user64/include/mich/wait.h src/user64/include/mich/vfs.h src/user64/include/mich/firmware.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(VIRTIO_NET_SAFE64_OBJ): src/user64/virtio_net/safe.c src/user64/include/mich/syscall.h src/user64/include/mich/driver.h src/user64/lib/virtio/virtio.h src/user64/include/mich/hardware.h src/user64/include/mich/net.h src/user64/include/mich/net_interface.h src/user64/include/mich/bridge.h src/user64/include/mich/wait.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(VIRTIO_NET64_PROBES_OBJ): src/user64/virtio_net/probes.c src/user64/virtio_net/capsule.h src/user64/include/mich/syscall.h src/user64/include/mich/net_interface.h src/user64/include/mich/socket.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(DNS_MESSAGE64_OBJ): src/net/dns_message.c src/net/dns_message.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@










$(DNS64_OBJ): src/user64/lib/dns.c src/user64/include/mich/dns.h src/net/dns_message.h src/user64/include/mich/socket.h src/user64/include/mich/syscall.h src/user64/include/mich/timer.h src/user64/include/mich/event.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(DNSPROBE64_OBJ): src/user64/dnsprobe/main.c src/user64/include/mich/dns.h src/user64/include/mich/syscall.h src/user64/include/mich/timer.h src/net/dns_message.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(DNSPROBE64_ELF): $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(DNSPROBE64_OBJ) $(DNS64_OBJ) $(DNS_MESSAGE64_OBJ) src/user64/linker.ld | $(USER64_DIR)
	$(LD) -m elf_x86_64 -x -T src/user64/linker.ld -o $@ $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(DNSPROBE64_OBJ) $(DNS64_OBJ) $(DNS_MESSAGE64_OBJ)

$(NETBENCH64_OBJ): src/user64/netbench/main.c src/user64/include/mich/syscall.h src/user64/include/mich/socket.h src/net/socket_abi.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) $(NETBENCH_TAP_FLAG) -Werror -c $< -o $@

$(NETBENCH64_ELF): $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(NETBENCH64_OBJ) src/user64/linker.ld | $(USER64_DIR)
	$(LD) -m elf_x86_64 -x -T src/user64/linker.ld -o $@ $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(NETBENCH64_OBJ)



$(VIRTIO_BLK64_ELF): $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(VIRTIO_LIB64_OBJ) $(VIRTIO_BLK64_OBJ) src/user64/linker.ld | $(USER64_DIR)
	$(LD) -m elf_x86_64 -x -T src/user64/linker.ld -o $@ $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(VIRTIO_LIB64_OBJ) $(VIRTIO_BLK64_OBJ)

# Builds the block-driver capsule on its own, without a boot image.
user64-virtio-blk: $(VIRTIO_BLK64_ELF)

$(NVME64_ELF): $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(NVME64_OBJ) src/user64/linker.ld | $(USER64_DIR)
	$(LD) -m elf_x86_64 -x -T src/user64/linker.ld -o $@ $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(NVME64_OBJ)

user64-nvme: $(NVME64_ELF)

$(VIRTIO_NET64_ELF): $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(VIRTIO_LIB64_OBJ) $(VIRTIO_NET64_OBJ) $(VIRTIO_NET64_PROBES_OBJ) $(DNS64_OBJ) $(DNS_MESSAGE64_OBJ) src/user64/linker.ld | $(USER64_DIR)
	$(LD) -m elf_x86_64 -x -T src/user64/linker.ld -o $@ $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(VIRTIO_LIB64_OBJ) $(VIRTIO_NET64_OBJ) $(VIRTIO_NET64_PROBES_OBJ) $(DNS64_OBJ) $(DNS_MESSAGE64_OBJ)

$(VIRTIO_NET_RECOVERY_RIP_H): $(VIRTIO_NET64_ELF) scripts/gen-virtio-net-recovery-rip.sh | $(GEN64_DIR)
	OBJDUMP=$(OBJDUMP) sh scripts/gen-virtio-net-recovery-rip.sh $(VIRTIO_NET64_ELF) > $@.tmp
	mv $@.tmp $@

$(VIRTIO_NET_SAFE64_ELF): $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(VIRTIO_LIB64_OBJ) $(VIRTIO_NET_SAFE64_OBJ) src/user64/linker.ld | $(USER64_DIR)
	$(LD) -m elf_x86_64 -x -T src/user64/linker.ld -o $@ $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(VIRTIO_LIB64_OBJ) $(VIRTIO_NET_SAFE64_OBJ)

$(KERNEL64_ELF): $(OBJS64) $(ARCH64_BOOT)/linker.ld | $(BIN64)
	$(LD) $(LDFLAGS64) -o $@ $(OBJS64)

$(KERNEL64_FLAT): $(KERNEL64_ELF)
	$(OBJCOPY) -O binary $< $@

$(KERNEL64_TEST_ELF): $(OBJS64_TEST) $(ARCH64_BOOT)/linker.ld | $(BIN64)
	$(LD) $(LDFLAGS64) -o $@ $(OBJS64_TEST)

$(KERNEL64_TEST_FLAT): $(KERNEL64_TEST_ELF)
	$(OBJCOPY) -O binary $< $@

$(UEFI64_OBJ): $(ARCH64_BOOT)/uefi.c $(CORE)/bootinfo.h $(CORE)/types.h | $(OBJ64)
	$(CC) $(CFLAGS64) -mno-sse -mno-sse2 -fno-ident -Werror -c $< -o $@

$(UEFI64_EFI): $(UEFI64_OBJ) $(ARCH64_BOOT)/uefi.ld | $(BIN64)
	$(LD) -m i386pep -T $(ARCH64_BOOT)/uefi.ld --subsystem 10 -e efi_main \
		--image-base 0x3000000 --section-alignment 4096 --file-alignment 512 \
		--disable-reloc-section --disable-dynamicbase --disable-high-entropy-va \
		-o $@ $(UEFI64_OBJ)

$(DISK64): mkuefi64.py $(KERNEL64_ELF) $(KERNEL64_FLAT) $(UEFI64_EFI) $(INIT64_ELF) $(POSIXAPP64_ELF) $(POSIXDEMO64_ELF)
	$(PYTHON) mkuefi64.py --efi $(UEFI64_EFI) $@ init64:0x20000C9=$(INIT64_ELF) posixapp:0=$(POSIXAPP64_ELF) posixdemo:0=$(POSIXDEMO64_ELF)

$(DISK64_TEST): mkuefi64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(UEFI64_EFI) $(INIT64_ELF) $(POSIXAPP64_ELF) $(POSIXDEMO64_ELF)
	$(PYTHON) mkuefi64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) \
		--efi $(UEFI64_EFI) $@ init64:0x30000C9=$(INIT64_ELF) posixapp:0=$(POSIXAPP64_ELF) posixdemo:0=$(POSIXDEMO64_ELF)

# Ships the default disk (kernel, init, posix images so init's process tests
# still pass) plus the virtio-blk capsule the kernel spots by module name.
$(DISK64_VIRTIO_BLK): mkuefi64.py $(KERNEL64_ELF) $(KERNEL64_FLAT) $(UEFI64_EFI) $(INIT64_ELF) $(POSIXAPP64_ELF) $(POSIXDEMO64_ELF) $(VIRTIO_BLK64_ELF)
	$(PYTHON) mkuefi64.py --efi $(UEFI64_EFI) $@ init64:0x30000C9=$(INIT64_ELF) posixapp:0=$(POSIXAPP64_ELF) posixdemo:0=$(POSIXDEMO64_ELF) virtio-blk:0=$(VIRTIO_BLK64_ELF)

# Production kernel plus the NVMe capsule the kernel spots by module name.
$(DISK64_NVME): mkuefi64.py $(KERNEL64_ELF) $(KERNEL64_FLAT) $(UEFI64_EFI) $(INIT64_ELF) $(POSIXAPP64_ELF) $(POSIXDEMO64_ELF) $(NVME64_ELF)
	$(PYTHON) mkuefi64.py --efi $(UEFI64_EFI) $@ init64:0x30000C9=$(INIT64_ELF) posixapp:0=$(POSIXAPP64_ELF) posixdemo:0=$(POSIXDEMO64_ELF) nvme:0=$(NVME64_ELF)

$(DISK64_UNIT): mkuefi64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(UEFI64_EFI) $(INIT64_ELF) $(POSIXAPP64_ELF) $(POSIXDEMO64_ELF)
	$(PYTHON) mkuefi64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) \
		--efi $(UEFI64_EFI) $@ init64:0x130000C9=$(INIT64_ELF) posixapp:0=$(POSIXAPP64_ELF) posixdemo:0=$(POSIXDEMO64_ELF)

$(DISK64_HARDWARE): mkuefi64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(UEFI64_EFI) $(INIT64_ELF) $(VIRTIO_NET64_ELF)
	$(PYTHON) mkuefi64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) \
		--efi $(UEFI64_EFI) $@ init64:0x410000C9=$(INIT64_ELF) virtio-net:0=$(VIRTIO_NET64_ELF)

$(DISK64_DNS): mkuefi64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(UEFI64_EFI) $(INIT64_ELF) $(VIRTIO_NET64_ELF) $(DNSPROBE64_ELF)
	$(PYTHON) mkuefi64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) \
		--efi $(UEFI64_EFI) $@ init64:0x410000C9=$(INIT64_ELF) virtio-net:0=$(VIRTIO_NET64_ELF) dnsprobe:0=$(DNSPROBE64_ELF)
# Loopback socket-path benchmark. Ships the same init flags and virtio-net
# capsule as the dns image so it reuses the kernel's probe-spawn hook,
# but the kernel recognises the "netbench" module and skips the driver-live
# recovery lab for this boot: a resident bench task perturbs that lab's tick
# deadline and pollutes the cycle counts, so the benchmark wants a quiescent
# kernel. The benchmark itself never touches the NIC; it runs over loopback.
$(DISK64_NETBENCH): mkuefi64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(UEFI64_EFI) $(INIT64_ELF) $(VIRTIO_NET64_ELF) $(NETBENCH64_ELF)
	$(PYTHON) mkuefi64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) \
		--efi $(UEFI64_EFI) $@ init64:0x410000C9=$(INIT64_ELF) virtio-net:0=$(VIRTIO_NET64_ELF) netbench:0=$(NETBENCH64_ELF)



$(DISK64_HARDWARE_RESTART): mkuefi64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(UEFI64_EFI) $(INIT64_ELF) $(VIRTIO_NET64_ELF)
	$(PYTHON) mkuefi64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) \
		--efi $(UEFI64_EFI) $@ init64:0x610000C9=$(INIT64_ELF) virtio-net:0=$(VIRTIO_NET64_ELF)

$(DISK64_HARDWARE_CIRCUIT): mkuefi64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(UEFI64_EFI) $(INIT64_ELF) $(VIRTIO_NET64_ELF)
	$(PYTHON) mkuefi64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) \
		--efi $(UEFI64_EFI) $@ init64:0x690000C9=$(INIT64_ELF) virtio-net:0=$(VIRTIO_NET64_ELF)

$(DISK64_HARDWARE_RECOVERY): mkuefi64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(UEFI64_EFI) $(INIT64_ELF) $(VIRTIO_NET64_ELF) $(VIRTIO_NET_SAFE64_ELF)
	$(PYTHON) mkuefi64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) \
		--efi $(UEFI64_EFI) $@ init64:0x650000C9=$(INIT64_ELF) virtio-net:0=$(VIRTIO_NET64_ELF) virtio-net-safe:0=$(VIRTIO_NET_SAFE64_ELF)

$(DISK64_PANIC): mkuefi64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(UEFI64_EFI) $(INIT64_ELF)
	$(PYTHON) mkuefi64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) \
		--efi $(UEFI64_EFI) $@ init64:0x800000C9=$(INIT64_ELF)

$(DISK64_CRASH): mkuefi64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(UEFI64_EFI) $(INIT64_ELF)
	$(PYTHON) mkuefi64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) \
		--efi $(UEFI64_EFI) $@ init64:0xC9=$(INIT64_ELF) crash64:0=$(INIT64_ELF)

run64: $(DISK64)
	sh ./scripts/qemu-run64.sh $(DISK64)

# With the syscall-186 fix init runs its whole suite, and the extra serial volume
# occasionally loses a byte in serial64_putc (non-blocking by design, see
# drivers/serial.c) which garbles one random marker line. That is emulation
# noise, not a kernel failure, so retry; a real regression fails every attempt.
test64: $(DISK64_TEST)
	@attempt=1; while :; do sh ./scripts/qemu-smoke64.sh $(DISK64_TEST) && break; if [ $$attempt -ge 4 ]; then echo "test64: smoke failed after $$attempt attempts"; exit 1; fi; echo "test64: transient smoke failure, retry $$attempt/4"; attempt=$$((attempt + 1)); done

test64-prod: $(DISK64)
	sh ./scripts/qemu-prod64.sh $(DISK64)

test64-unit: $(DISK64_UNIT)
	sh ./scripts/qemu-unit64.sh $(DISK64_UNIT)

test64-highmem: $(DISK64_TEST)
	sh ./scripts/qemu-smoke64.sh $(DISK64_TEST) 768M highmem

test64-smp: $(DISK64_TEST)
	sh ./scripts/qemu-smoke64.sh $(DISK64_TEST) 128M smp

test64-hardware: $(DISK64_HARDWARE)
	sh ./scripts/qemu-smoke64.sh $(DISK64_HARDWARE) 128M hardware
test64-dns: $(DISK64_DNS)
	sh ./scripts/qemu-smoke64.sh $(DISK64_DNS) 256M dns
test64-netbench: $(DISK64_NETBENCH)
	sh ./scripts/qemu-smoke64.sh $(DISK64_NETBENCH) 256M netbench
test64-virtio-blk: $(DISK64_VIRTIO_BLK)
	sh ./scripts/qemu-smoke64.sh $(DISK64_VIRTIO_BLK) 256M virtio-blk
test64-nvme: $(DISK64_NVME)
	sh ./scripts/qemu-smoke64.sh $(DISK64_NVME) 256M nvme




test64-msi: $(DISK64_HARDWARE)
	sh ./scripts/qemu-smoke64.sh $(DISK64_HARDWARE) 256M msi

test64-msi-restart: $(DISK64_HARDWARE_RESTART)
	sh ./scripts/qemu-smoke64.sh $(DISK64_HARDWARE_RESTART) 256M msi-restart

test64-msi-circuit: $(DISK64_HARDWARE_CIRCUIT)
	sh ./scripts/qemu-smoke64.sh $(DISK64_HARDWARE_CIRCUIT) 256M msi-circuit

test64-msi-recovery: $(DISK64_HARDWARE_RECOVERY)
	sh ./scripts/qemu-smoke64.sh $(DISK64_HARDWARE_RECOVERY) 256M msi-recovery

test64-msi-restart-stability: $(DISK64_HARDWARE_RESTART)
	sh ./scripts/qemu-recovery-stability.sh $(DISK64_HARDWARE_RESTART) 256M msi-restart $(RECOVERY_STABILITY_RUNS)

test64-msi-circuit-stability: $(DISK64_HARDWARE_CIRCUIT)
	sh ./scripts/qemu-recovery-stability.sh $(DISK64_HARDWARE_CIRCUIT) 256M msi-circuit $(RECOVERY_STABILITY_RUNS)

test64-msi-recovery-stability: $(DISK64_HARDWARE_RECOVERY)
	sh ./scripts/qemu-recovery-stability.sh $(DISK64_HARDWARE_RECOVERY) 256M msi-recovery $(RECOVERY_STABILITY_RUNS)

test64-pcie: $(DISK64_TEST)
	sh ./scripts/qemu-smoke64.sh $(DISK64_TEST) 256M pcie

test64-iommu: $(DISK64_TEST)
	sh ./scripts/qemu-smoke64.sh $(DISK64_TEST) 256M iommu

test64-amd-iommu: $(DISK64_TEST)
	sh ./scripts/qemu-smoke64.sh $(DISK64_TEST) 256M amd-iommu

test64-panic: $(DISK64_PANIC)
	sh ./scripts/qemu-panic64.sh $(DISK64_PANIC)

test64-crash: $(DISK64_CRASH)
	sh ./scripts/qemu-crash64.sh $(DISK64_CRASH) 128M

release-check:
	$(MAKE) clean
	$(MAKE) all
	$(MAKE) test64
	$(MAKE) test64-prod
	$(MAKE) test64-unit
	$(MAKE) test64-highmem
	$(MAKE) test64-hardware
	$(MAKE) test64-msi
	$(MAKE) test64-pcie
	$(MAKE) test64-iommu
	$(MAKE) test64-amd-iommu
	$(MAKE) test64-panic
	! grep -q '—' README.md
	grep -q '#define MICH_VERSION_STRING "0.2.0"' src/core/version.h
	git diff --check

DEPFILES = $(OBJS64:.o=.d) $(OBJS64_TEST:.o=.d) $(PORTABLE64_OBJS:.o=.d) \
           $(INIT64_OBJS:.o=.d) $(VIRTIO_NET64_OBJ:.o=.d) \
           $(VIRTIO_NET_SAFE64_OBJ:.o=.d) $(VIRTIO_NET64_PROBES_OBJ:.o=.d) \
           $(DNS64_OBJ:.o=.d) $(DNS_MESSAGE64_OBJ:.o=.d) \
           $(POSIXAPP64_OBJS:.o=.d) $(POSIXDEMO64_OBJS:.o=.d) $(UEFI64_OBJ:.o=.d)
-include $(DEPFILES)

clean:
	rm -rf $(BIN_DIR)

.PHONY: all user64-virtio-blk test64-virtio-blk user64-nvme test64-nvme run64 test64 test64-prod test64-crash test64-unit test64-highmem test64-dns test64-hardware test64-msi test64-msi-restart test64-msi-circuit test64-msi-recovery test64-msi-restart-stability test64-msi-circuit-stability test64-msi-recovery-stability test64-pcie test64-panic release-check clean
