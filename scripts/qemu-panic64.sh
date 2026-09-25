#!/bin/sh
set -eu
image="${1:-bin/x86_64/disk-panic.img}"
. "$(dirname "$0")/uefi-firmware.sh"
mich_uefi_firmware
log="$(mktemp)"
trap 'rm -f "$log" "$uefi_vars"' EXIT
set +e
timeout 35s qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64,+aes,+pclmulqdq,+ssse3 \
    -drive if=pflash,format=raw,readonly=on,file="$uefi_code" \
    -drive if=pflash,format=raw,file="$uefi_vars" \
    -drive file="$image",format=raw,if=none,id=esdisk \
    -device ide-hd,drive=esdisk,bootindex=1 \
    -m 128M \
    -serial stdio \
    -display none \
    -no-reboot \
    -no-shutdown >"$log" 2>&1
status=$?
set -e
if [ "$status" -ne 0 ] && [ "$status" -ne 124 ]; then
    cat "$log"
    exit 1
fi
for marker in \
    "Mich Core 0.1.0 x86_64: long mode alive" \
    "Mich x86_64: IDT alive" \
    "Mich x86_64: panic subsystem ready" \
    "Mich x86_64: panic profile trigger" \
    "MICH KERNEL PANIC" \
    "reason: kernel exception" \
    "task slot: 0x" \
    "task pid:  0x" \
    "cr2:       0x0000000DEADBE000" \
    "cr3:       0x" \
    "vector:    0x000000000000000E" \
    "error:     0x0000000000000000" \
    "rip:       0x" \
    "cs:        0x" \
    "rflags:    0x" \
    "rsp:       0x" \
    "ss:        0x" \
    "rax:       0x" \
    "rbx:       0x" \
    "rcx:       0x" \
    "rdx:       0x" \
    "rsi:       0x" \
    "rdi:       0x" \
    "rbp:       0x" \
    "r8:        0x" \
    "r9:        0x" \
    "r10:       0x" \
    "r11:       0x" \
    "r12:       0x" \
    "r13:       0x" \
    "r14:       0x" \
    "r15:       0x"
do
    grep -Fq "$marker" "$log" || { cat "$log"; exit 1; }
done
if grep -Fq "MICH RECURSIVE PANIC" "$log" ||
   grep -Fq "panic profile did not fault" "$log" ||
   grep -Fq "Mich x86_64: userspace runtime pass" "$log"; then
    cat "$log"
    exit 1
fi
cat "$log"
echo "qemu-panic64: PASS"
