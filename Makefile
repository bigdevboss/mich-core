CC = gcc
AS = nasm
LD = ld

CFLAGS = -m32 -nostdlib -nostdinc -fno-builtin -fno-stack-protector -nostartfiles -nodefaultlibs -ffreestanding -Wall -Wextra -O2
ASFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T src/linker.ld

OBJS = src/boot.o src/kernel.o src/gdt.o src/idt.o src/irq.o src/gfx.o src/interrupt.o src/exceptions.o src/timer.o src/paging.o src/pmm.o src/task.o src/tss.o src/syscall.o src/syscall_asm.o src/double_fault.o src/elf.o src/scheduler.o src/ipc.o src/proc.o src/serial.o

mich-kernel.bin: $(OBJS)
	$(LD) $(LDFLAGS) -o mich-kernel.bin $(OBJS)

kflat.bin: mich-kernel.bin
	objcopy -O binary mich-kernel.bin kflat.bin

bdb1.bin: src/bdb1.asm
	$(AS) -f bin src/bdb1.asm -o bdb1.bin

bdb2.bin: src/bdb2.asm
	$(AS) -f bin src/bdb2.asm -o bdb2.bin

disk.img: mkboot.py mich-kernel.bin bdb1.bin bdb2.bin kflat.bin
	python3 mkboot.py disk.img

all: disk.img

run: disk.img
	qemu-system-i386 -drive file=disk.img,format=raw,if=ide,index=0,media=disk -boot c -serial stdio

src/boot.o: src/boot.asm
	$(AS) $(ASFLAGS) src/boot.asm -o src/boot.o

src/interrupt.o: src/interrupt.asm
	$(AS) $(ASFLAGS) src/interrupt.asm -o src/interrupt.o

src/exceptions.o: src/exceptions.asm
	$(AS) $(ASFLAGS) src/exceptions.asm -o src/exceptions.o

src/timer.o: src/timer.asm
	$(AS) $(ASFLAGS) src/timer.asm -o src/timer.o

src/syscall_asm.o: src/syscall_asm.asm
	$(AS) $(ASFLAGS) src/syscall_asm.asm -o src/syscall_asm.o

src/double_fault.o: src/double_fault.asm
	$(AS) $(ASFLAGS) src/double_fault.asm -o src/double_fault.o

src/kernel.o: src/kernel.c
	$(CC) $(CFLAGS) -c src/kernel.c -o src/kernel.o

src/gdt.o: src/gdt.c
	$(CC) $(CFLAGS) -c src/gdt.c -o src/gdt.o

src/idt.o: src/idt.c
	$(CC) $(CFLAGS) -c src/idt.c -o src/idt.o

src/irq.o: src/irq.c
	$(CC) $(CFLAGS) -c src/irq.c -o src/irq.o

src/gfx.o: src/gfx.c src/font.h
	$(CC) $(CFLAGS) -c src/gfx.c -o src/gfx.o

src/paging.o: src/paging.c
	$(CC) $(CFLAGS) -c src/paging.c -o src/paging.o

src/pmm.o: src/pmm.c
	$(CC) $(CFLAGS) -c src/pmm.c -o src/pmm.o

src/task.o: src/task.c
	$(CC) $(CFLAGS) -c src/task.c -o src/task.o

src/tss.o: src/tss.c
	$(CC) $(CFLAGS) -c src/tss.c -o src/tss.o

src/syscall.o: src/syscall.c
	$(CC) $(CFLAGS) -c src/syscall.c -o src/syscall.o

src/elf.o: src/elf.c
	$(CC) $(CFLAGS) -c src/elf.c -o src/elf.o

src/scheduler.o: src/scheduler.c
	$(CC) $(CFLAGS) -c src/scheduler.c -o src/scheduler.o

src/ipc.o: src/ipc.c
	$(CC) $(CFLAGS) -c src/ipc.c -o src/ipc.o

src/proc.o: src/proc.c
	$(CC) $(CFLAGS) -c src/proc.c -o src/proc.o

src/serial.o: src/serial.c
	$(CC) $(CFLAGS) -c src/serial.c -o src/serial.o

clean:
	rm -f src/*.o mich-kernel.bin mich.iso kflat.bin bdb1.bin bdb2.bin disk.img
