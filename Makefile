.DEFAULT_GOAL := all

CC = gcc
AS = nasm
LD = ld
OBJCOPY = objcopy
PYTHON = python3

ARCH32 = src/arch/x86/i386
CORE = src/core
PROCESS = src/process
OBJECTS = src/objects
NET = src/net
DRIVER = src/driver
FS = src/fs
BLOCK = src/block
PORTABLE_INCLUDES = -Isrc/arch -I$(CORE) -I$(PROCESS) -I$(OBJECTS) \
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
CFLAGS = -m32 -mno-sse -mno-sse2 -mno-mmx -mno-3dnow -nostdlib -nostdinc -fno-builtin -fno-stack-protector -nostartfiles -nodefaultlibs -ffreestanding -fno-pie -fno-pic -fno-asynchronous-unwind-tables -MMD -MP -Wall -Wextra -O2 -Isrc $(PORTABLE_INCLUDES) -I$(ARCH32)
USER_CFLAGS = $(CFLAGS) -Isrc/user/include
ASFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T $(ARCH32)/linker.ld
USER_LDFLAGS = -m elf_i386 -T src/user/linker.ld
# x86-64 baseline hardware includes SSE2; it is used for payload and
# checksum copies. The i386 build keeps SSE disabled (not guaranteed there).
# proc.c, ipc.c, uaccess.c, and scheduler.c still use the i386 paging/elf
# API; they are not part of the x86-64 kernel. Fork/exec on x86-64 live in
# vm64/kernel.c.
# GCC's own include directory provides the SSE2 intrinsics headers, which
# are self-contained under -nostdinc.
GCC_INCLUDE = $(shell $(CC) -print-file-name=include)
CFLAGS64 = -m64 -mno-red-zone -msse2 -mno-mmx -nostdlib -nostdinc -fno-builtin -fno-stack-protector -nostartfiles -nodefaultlibs -ffreestanding -fno-pie -fno-pic -fno-asynchronous-unwind-tables -MMD -MP -Wall -Wextra -O2 -Isrc -I$(GCC_INCLUDE) $(ARCH64_INCLUDES) $(PORTABLE_INCLUDES) -I$(TEST64)
USER64_CFLAGS = $(CFLAGS64) -mcmodel=large -Isrc/user64/include
LDFLAGS64 = -m elf_x86_64 -T $(ARCH64_BOOT)/linker.ld

BIN_DIR = bin
OBJ_DIR = $(BIN_DIR)/obj
USER_DIR = $(BIN_DIR)/user
USER_OBJ_DIR = $(USER_DIR)/obj
KERNEL_ELF = $(BIN_DIR)/mich-kernel.elf
KERNEL_FLAT = $(BIN_DIR)/mich-kernel.bin
BDB1 = $(BIN_DIR)/bdb1.bin
BDB2 = $(BIN_DIR)/bdb2.bin
DISK_IMAGE = $(BIN_DIR)/disk.img
INIT_ELF = $(USER_DIR)/init.elf
INIT_OBJS = $(USER_OBJ_DIR)/crt0.o $(USER_OBJ_DIR)/syscall.o $(USER_OBJ_DIR)/init.o
BIN64 = $(BIN_DIR)/x86_64
OBJ64 = $(BIN64)/obj
KERNEL64_ELF = $(BIN64)/mich-kernel.elf
KERNEL64_FLAT = $(BIN64)/mich-kernel.bin
KERNEL64_TEST_ELF = $(BIN64)/mich-kernel-test.elf
KERNEL64_TEST_FLAT = $(BIN64)/mich-kernel-test.bin
BDB1_64 = $(BIN64)/bdb1.bin
BDB2_64 = $(BIN64)/bdb2.bin
DISK64 = $(BIN64)/disk.img
DISK64_TEST = $(BIN64)/disk-test.img
DISK64_UNIT = $(BIN64)/disk-unit.img
DISK64_HARDWARE = $(BIN64)/disk-hardware.img
DISK64_HARDWARE_RESTART = $(BIN64)/disk-hardware-restart.img
DISK64_HARDWARE_CIRCUIT = $(BIN64)/disk-hardware-circuit.img
DISK64_PANIC = $(BIN64)/disk-panic.img
UEFI64_OBJ = $(OBJ64)/uefi.o
UEFI64_EFI = $(BIN64)/BOOTX64.EFI
DISK64_UEFI = $(BIN64)/disk-uefi.img
DISK64_UEFI_TEST = $(BIN64)/disk-uefi-test.img
USER64_DIR = $(BIN64)/user
USER64_OBJ_DIR = $(USER64_DIR)/obj
INIT64_ELF = $(USER64_DIR)/init64.elf
INIT64_OBJS = $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(USER64_OBJ_DIR)/init.o
VIRTIO_NET64_OBJ = $(USER64_OBJ_DIR)/virtio_net.o
VIRTIO_NET64_PROBES_OBJ = $(USER64_OBJ_DIR)/virtio_net_probes.o
VIRTIO_NET64_ELF = $(USER64_DIR)/virtio-net.elf
OBJS64 = $(OBJ64)/boot.o $(OBJ64)/kernel.o $(OBJ64)/task_core.o $(OBJ64)/scheduler_core.o $(OBJ64)/service_core.o $(OBJ64)/object_core.o $(OBJ64)/resource_core.o $(OBJ64)/iommu_core.o $(OBJ64)/driver_core.o $(OBJ64)/driver_supervisor_core.o $(OBJ64)/driver_manager_core.o $(OBJ64)/ring_core.o $(OBJ64)/completion_core.o $(OBJ64)/timer_object_core.o $(OBJ64)/net_buffer_core.o $(OBJ64)/vnic_core.o $(OBJ64)/vnic_benchmark.o $(OBJ64)/net_interface_core.o $(OBJ64)/ethernet_core.o $(OBJ64)/arp_core.o $(OBJ64)/ipv4_core.o $(OBJ64)/ipv6_core.o $(OBJ64)/icmp_core.o $(OBJ64)/icmpv6_core.o $(OBJ64)/loopback_core.o $(OBJ64)/udp_core.o $(OBJ64)/udpv6_core.o $(OBJ64)/tcp_core.o $(OBJ64)/tcp_cc_core.o $(OBJ64)/pmtu_core.o $(OBJ64)/route_core.o $(OBJ64)/socket_core.o $(OBJ64)/vfs_core.o $(OBJ64)/blockfs_core.o $(OBJ64)/block_core.o $(OBJ64)/cache_core.o $(OBJ64)/firmware_core.o $(OBJ64)/event_core.o $(OBJ64)/endpoint_core.o $(OBJ64)/bridge_core.o $(OBJ64)/pmm_core.o $(OBJ64)/mem_core.o $(OBJ64)/ipc64.o $(OBJ64)/acpi64.o $(OBJ64)/vtd64.o $(OBJ64)/amd_iommu64.o $(OBJ64)/pci64.o $(OBJ64)/virtio_pci.o $(OBJ64)/virtio_blk.o $(OBJ64)/apic64.o $(OBJ64)/ioapic64.o $(OBJ64)/smp64.o $(OBJ64)/smp_tramp.o $(OBJ64)/vector64.o $(OBJ64)/msi64.o $(OBJ64)/msix64.o $(OBJ64)/panic64.o $(OBJ64)/gdt_asm.o $(OBJ64)/gdt.o $(OBJ64)/exceptions.o $(OBJ64)/interrupt.o $(OBJ64)/idt.o $(OBJ64)/vm.o $(OBJ64)/elf64.o $(OBJ64)/platform.o $(OBJ64)/serial.o $(OBJ64)/syscall_dispatch.o $(OBJ64)/syscall.o
TEST64_OBJS = $(OBJ64)/test_runner64.o $(OBJ64)/test_object64.o $(OBJ64)/test_resource64.o $(OBJ64)/test_async64.o $(OBJ64)/test_fpu64.o $(OBJ64)/test_smp64.o $(OBJ64)/test_driver64.o $(OBJ64)/test_hardware64.o $(OBJ64)/test_network_runner64.o $(OBJ64)/test_net_support64.o $(OBJ64)/test_net_foundation64.o $(OBJ64)/test_ipv4_64.o $(OBJ64)/test_ipv6_64.o $(OBJ64)/test_tcp64.o $(OBJ64)/test_udp_socket64.o $(OBJ64)/test_net_interface64.o $(OBJ64)/test_vfs64.o $(OBJ64)/test_block64.o $(OBJ64)/test_cache64.o $(OBJ64)/test_blockfs64.o $(OBJ64)/test_virtio_blk64.o $(OBJ64)/test_net_bench64.o $(OBJ64)/test_report64.o
OBJS64_TEST = $(OBJ64)/boot.o $(OBJ64)/kernel_test.o $(filter-out $(OBJ64)/boot.o $(OBJ64)/kernel.o,$(OBJS64)) $(TEST64_OBJS)
PORTABLE64_DIR = $(BIN64)/portable
PORTABLE64_NAMES = task service object resource driver driver_supervisor driver_manager ring completion timer_object net_buffer vnic net_interface ethernet arp ipv4 ipv6 icmp icmpv6 loopback udp udpv6 tcp tcp_cc pmtu route socket vfs blockfs block cache firmware event endpoint bridge pmm
PORTABLE64_OBJS = $(addprefix $(PORTABLE64_DIR)/,$(addsuffix .o,$(PORTABLE64_NAMES)))

OBJ_NAMES = boot kernel gdt idt irq gfx interrupt exceptions exception timer paging pmm mem uaccess task tss syscall syscall_asm double_fault elf scheduler scheduler_core ipc proc service object resource driver driver_supervisor driver_manager ring completion timer_object net_buffer vnic net_interface ethernet arp ipv4 ipv6 icmp icmpv6 loopback udp udpv6 tcp tcp_cc pmtu route socket vfs firmware event endpoint bridge platform serial
OBJS = $(addprefix $(OBJ_DIR)/,$(addsuffix .o,$(OBJ_NAMES)))

all: $(DISK_IMAGE) $(DISK64) $(PORTABLE64_OBJS)

$(BIN_DIR) $(OBJ_DIR) $(USER_DIR) $(USER_OBJ_DIR) $(BIN64) $(OBJ64) $(PORTABLE64_DIR) $(USER64_DIR) $(USER64_OBJ_DIR):
	mkdir -p $@

$(KERNEL_ELF): $(OBJS) $(ARCH32)/linker.ld | $(BIN_DIR)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(KERNEL_FLAT): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

$(BDB1): $(ARCH32)/bdb1.asm | $(BIN_DIR)
	$(AS) -f bin $< -o $@

$(BDB2): $(ARCH32)/bdb2.asm | $(BIN_DIR)
	$(AS) -f bin $< -o $@

$(USER_OBJ_DIR)/crt0.o: src/user/lib/crt0.asm | $(USER_OBJ_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_OBJ_DIR)/syscall.o: src/user/lib/syscall.asm | $(USER_OBJ_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_OBJ_DIR)/init.o: src/user/init/main.c src/user/include/mich/syscall.h src/user/include/mich/service.h src/user/include/mich/exception.h src/user/include/mich/ipc.h src/user/include/mich/capability.h | $(USER_OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(INIT_ELF): $(INIT_OBJS) src/user/linker.ld | $(USER_DIR)
	$(LD) $(USER_LDFLAGS) -o $@ $(INIT_OBJS)

$(DISK_IMAGE): mkboot.py $(KERNEL_ELF) $(KERNEL_FLAT) $(BDB1) $(BDB2) $(INIT_ELF)
	$(PYTHON) mkboot.py $@ init:0x59=$(INIT_ELF)

$(OBJ_DIR)/%.o: $(CORE)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(PROCESS)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(OBJECTS)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(NET)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(DRIVER)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(FS)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(ARCH32)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(ARCH32)/%.asm | $(OBJ_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/gfx.o: $(ARCH32)/font.h
$(OBJ_DIR)/kernel.o: $(CORE)/version.h

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

$(OBJ64)/kernel.o: $(ARCH64_KERNEL)/kernel.c $(CORE)/version.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/kernel_test.o: $(ARCH64_KERNEL)/kernel.c $(CORE)/version.h $(TEST64)/tests64.h $(TEST64)/test_report.h | $(OBJ64)
	$(CC) $(CFLAGS64) -DMICH_TEST_BUILD -Werror -c $< -o $@

$(OBJ64)/syscall_dispatch.o: $(ARCH64_KERNEL)/syscall_dispatch.c $(ARCH64_KERNEL)/kernel64_internal.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/task_core.o: $(PROCESS)/task.c $(PROCESS)/task.h src/arch/arch_task.h | $(OBJ64)
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

$(OBJ64)/net_interface_core.o: $(NET)/net_interface.c $(NET)/net_interface.h $(DRIVER)/driver_supervisor.h | $(OBJ64)
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

$(OBJ64)/tcp_core.o: $(NET)/tcp.c $(NET)/tcp.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/tcp_cc_core.o: $(NET)/tcp_cc.c $(NET)/tcp.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/pmtu_core.o: $(NET)/pmtu.c $(NET)/pmtu.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/route_core.o: $(NET)/route.c $(NET)/route.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/socket_core.o: $(NET)/socket.c $(NET)/socket.h $(NET)/udp.h $(NET)/route.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/vfs_core.o: $(FS)/vfs.c $(FS)/vfs.h $(FS)/blockfs.h $(OBJECTS)/object.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/blockfs_core.o: $(FS)/blockfs.c $(FS)/blockfs.h $(FS)/vfs.h $(BLOCK)/block.h $(BLOCK)/cache.h | $(OBJ64)
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

$(OBJ64)/test_udp_socket64.o: $(TEST64)/udp_socket_test.c $(TEST64)/tests64.h $(TEST64)/net_test.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_net_interface64.o: $(TEST64)/net_interface_test.c $(TEST64)/tests64.h $(TEST64)/net_test.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_vfs64.o: $(TEST64)/vfs_test.c $(TEST64)/tests64.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_block64.o: $(TEST64)/block_test.c $(TEST64)/tests64.h $(BLOCK)/block.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_cache64.o: $(TEST64)/cache_test.c $(TEST64)/tests64.h $(BLOCK)/cache.h $(BLOCK)/block.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_blockfs64.o: $(TEST64)/blockfs_test.c $(TEST64)/tests64.h $(FS)/vfs.h $(FS)/blockfs.h $(BLOCK)/block.h | $(OBJ64)
	$(CC) $(CFLAGS64) -Werror -c $< -o $@

$(OBJ64)/test_virtio_blk64.o: $(TEST64)/virtio_blk_test.c $(TEST64)/tests64.h $(ARCH64_DRIVERS)/virtio_blk.h $(BLOCK)/block.h | $(OBJ64)
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

$(USER64_OBJ_DIR)/init.o: src/user64/init/main.c src/user64/include/mich/syscall.h src/user64/include/mich/service.h src/user64/include/mich/capability.h src/user64/include/mich/ipc.h src/user64/include/mich/event.h src/user64/include/mich/bridge.h src/user64/include/mich/hardware.h src/user64/include/mich/driver.h src/user64/include/mich/memory.h src/user64/include/mich/sg.h src/user64/include/mich/ring.h src/user64/include/mich/completion.h src/user64/include/mich/timer.h src/user64/include/mich/wait.h src/user64/include/mich/net.h src/user64/include/mich/socket.h src/user64/include/mich/net_interface.h src/user64/include/mich/virtio.h src/user64/include/mich/vfs.h src/user64/include/mich/firmware.h src/user64/include/mich/block.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(INIT64_ELF): $(INIT64_OBJS) src/user64/linker.ld | $(USER64_DIR)
	$(LD) -m elf_x86_64 -T src/user64/linker.ld -o $@ $(INIT64_OBJS)

$(VIRTIO_NET64_OBJ): src/user64/virtio_net/main.c src/user64/virtio_net/capsule.h src/user64/include/mich/syscall.h src/user64/include/mich/event.h src/user64/include/mich/driver.h src/user64/include/mich/virtio.h src/user64/include/mich/net.h src/user64/include/mich/net_interface.h src/user64/include/mich/timer.h src/user64/include/mich/wait.h src/user64/include/mich/vfs.h src/user64/include/mich/firmware.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(VIRTIO_NET64_PROBES_OBJ): src/user64/virtio_net/probes.c src/user64/virtio_net/capsule.h src/user64/include/mich/syscall.h src/user64/include/mich/net_interface.h src/user64/include/mich/socket.h | $(USER64_OBJ_DIR)
	$(CC) $(USER64_CFLAGS) -Werror -c $< -o $@

$(VIRTIO_NET64_ELF): $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(VIRTIO_NET64_OBJ) $(VIRTIO_NET64_PROBES_OBJ) src/user64/linker.ld | $(USER64_DIR)
	$(LD) -m elf_x86_64 -x -T src/user64/linker.ld -o $@ $(USER64_OBJ_DIR)/crt0.o $(USER64_OBJ_DIR)/syscall.o $(VIRTIO_NET64_OBJ) $(VIRTIO_NET64_PROBES_OBJ)

$(KERNEL64_ELF): $(OBJS64) $(ARCH64_BOOT)/linker.ld | $(BIN64)
	$(LD) $(LDFLAGS64) -o $@ $(OBJS64)

$(KERNEL64_FLAT): $(KERNEL64_ELF)
	$(OBJCOPY) -O binary $< $@

$(KERNEL64_TEST_ELF): $(OBJS64_TEST) $(ARCH64_BOOT)/linker.ld | $(BIN64)
	$(LD) $(LDFLAGS64) -o $@ $(OBJS64_TEST)

$(KERNEL64_TEST_FLAT): $(KERNEL64_TEST_ELF)
	$(OBJCOPY) -O binary $< $@

$(BDB1_64): $(ARCH64_BOOT)/bdb1.asm | $(BIN64)
	$(AS) -f bin $< -o $@

$(BDB2_64): $(ARCH64_BOOT)/bdb2.asm | $(BIN64)
	$(AS) -f bin $< -o $@

$(UEFI64_OBJ): $(ARCH64_BOOT)/uefi.c $(CORE)/bootinfo.h $(CORE)/types.h | $(OBJ64)
	$(CC) $(CFLAGS64) -mno-sse -mno-sse2 -fno-ident -Werror -c $< -o $@

$(UEFI64_EFI): $(UEFI64_OBJ) $(ARCH64_BOOT)/uefi.ld | $(BIN64)
	$(LD) -m i386pep -T $(ARCH64_BOOT)/uefi.ld --subsystem 10 -e efi_main \
		--image-base 0x3000000 --section-alignment 4096 --file-alignment 512 \
		--disable-reloc-section --disable-dynamicbase --disable-high-entropy-va \
		-o $@ $(UEFI64_OBJ)

$(DISK64_UEFI): mkuefi64.py mkboot64.py $(KERNEL64_ELF) $(KERNEL64_FLAT) $(UEFI64_EFI) $(INIT64_ELF)
	$(PYTHON) mkuefi64.py --efi $(UEFI64_EFI) $@ init64:0xC9=$(INIT64_ELF)

$(DISK64_UEFI_TEST): mkuefi64.py mkboot64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(UEFI64_EFI) $(INIT64_ELF)
	$(PYTHON) mkuefi64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) \
		--efi $(UEFI64_EFI) $@ init64:0xC9=$(INIT64_ELF)

$(DISK64): mkboot64.py $(KERNEL64_ELF) $(KERNEL64_FLAT) $(BDB1_64) $(BDB2_64) $(INIT64_ELF)
	$(PYTHON) mkboot64.py $@ init64:0xC9=$(INIT64_ELF)

$(DISK64_TEST): mkboot64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(BDB1_64) $(BDB2_64) $(INIT64_ELF)
	$(PYTHON) mkboot64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) $@ init64:0xC9=$(INIT64_ELF)

$(DISK64_UNIT): mkboot64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(BDB1_64) $(BDB2_64) $(INIT64_ELF)
	$(PYTHON) mkboot64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) $@ init64:0x100000C9=$(INIT64_ELF)

$(DISK64_HARDWARE): mkboot64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(BDB1_64) $(BDB2_64) $(INIT64_ELF) $(VIRTIO_NET64_ELF)
	$(PYTHON) mkboot64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) $@ init64:0x400000C9=$(INIT64_ELF) virtio-net:0=$(VIRTIO_NET64_ELF)

$(DISK64_HARDWARE_RESTART): mkboot64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(BDB1_64) $(BDB2_64) $(INIT64_ELF) $(VIRTIO_NET64_ELF)
	$(PYTHON) mkboot64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) $@ init64:0x600000C9=$(INIT64_ELF) virtio-net:0=$(VIRTIO_NET64_ELF)

$(DISK64_HARDWARE_CIRCUIT): mkboot64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(BDB1_64) $(BDB2_64) $(INIT64_ELF) $(VIRTIO_NET64_ELF)
	$(PYTHON) mkboot64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) $@ init64:0x680000C9=$(INIT64_ELF) virtio-net:0=$(VIRTIO_NET64_ELF)

$(DISK64_PANIC): mkboot64.py $(KERNEL64_TEST_ELF) $(KERNEL64_TEST_FLAT) $(BDB1_64) $(BDB2_64) $(INIT64_ELF)
	$(PYTHON) mkboot64.py --kernel $(KERNEL64_TEST_ELF) --kernel-flat $(KERNEL64_TEST_FLAT) $@ init64:0x800000C9=$(INIT64_ELF)

run: $(DISK_IMAGE)
	qemu-system-i386 -drive file=$(DISK_IMAGE),format=raw,if=ide,index=0,media=disk -boot c -serial stdio

test: $(DISK_IMAGE)
	sh ./scripts/qemu-smoke.sh $(DISK_IMAGE)

run64: $(DISK64)
	qemu-system-x86_64 -drive file=$(DISK64),format=raw,if=ide,index=0,media=disk -boot c -serial stdio

test64: $(DISK64_TEST)
	sh ./scripts/qemu-smoke64.sh $(DISK64_TEST)

test64-uefi: $(DISK64_UEFI_TEST)
	sh ./scripts/qemu-smoke64.sh $(DISK64_UEFI_TEST) 128M uefi

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

test64-msi: $(DISK64_HARDWARE)
	sh ./scripts/qemu-smoke64.sh $(DISK64_HARDWARE) 256M msi

test64-msi-restart: $(DISK64_HARDWARE_RESTART)
	sh ./scripts/qemu-smoke64.sh $(DISK64_HARDWARE_RESTART) 256M msi-restart

test64-msi-circuit: $(DISK64_HARDWARE_CIRCUIT)
	sh ./scripts/qemu-smoke64.sh $(DISK64_HARDWARE_CIRCUIT) 256M msi-circuit

test64-pcie: $(DISK64_TEST)
	sh ./scripts/qemu-smoke64.sh $(DISK64_TEST) 256M pcie

test64-iommu: $(DISK64_TEST)
	sh ./scripts/qemu-smoke64.sh $(DISK64_TEST) 256M iommu

test64-amd-iommu: $(DISK64_TEST)
	sh ./scripts/qemu-smoke64.sh $(DISK64_TEST) 256M amd-iommu

test64-panic: $(DISK64_PANIC)
	sh ./scripts/qemu-panic64.sh $(DISK64_PANIC)

release-check:
	$(MAKE) clean
	$(MAKE) all
	$(MAKE) test
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
	grep -q '#define MICH_VERSION_STRING "0.1.0"' src/core/version.h
	git diff --check

DEPFILES = $(OBJS:.o=.d) $(OBJS64:.o=.d) $(OBJS64_TEST:.o=.d) $(PORTABLE64_OBJS:.o=.d) \
           $(INIT_OBJS:.o=.d) $(INIT64_OBJS:.o=.d) $(VIRTIO_NET64_OBJ:.o=.d) \
           $(VIRTIO_NET64_PROBES_OBJ:.o=.d) $(UEFI64_OBJ:.o=.d)
-include $(DEPFILES)

clean:
	rm -rf $(BIN_DIR)

.PHONY: all run test run64 test64 test64-uefi test64-prod test64-unit test64-highmem test64-hardware test64-msi test64-msi-restart test64-msi-circuit test64-pcie test64-panic release-check clean
