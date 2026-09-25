#!/bin/sh
set -eu
image="${1:-bin/x86_64/disk.img}"
. "$(dirname "$0")/uefi-firmware.sh"
mich_uefi_firmware
log="$(mktemp)"
trap 'rm -f "$log" "$uefi_vars"' EXIT
set +e
timeout 40s qemu-system-x86_64 \
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
    "Mich x86_64: PML4 address space alive" \
    "Mich x86_64: network runtime ready" \
    "Mich x86_64: external init64 task one" \
    "Mich x86_64: userspace runtime pass" \
    "Mich x86_64: POSIX userspace facade pass" \
    "Mich x86_64: POSIX userspace process pass" \
    "Mich x86_64: POSIX static application pass" \
    "Mich x86_64: syscall/sysret pass"
do
    grep -Fq "$marker" "$log" || {
        cat "$log"
        echo "qemu-prod64: missing marker: $marker" >&2
        exit 1
    }
done
if grep -Fq "Mich test64:" "$log"; then
    cat "$log"
    echo "qemu-prod64: production image contains test output" >&2
    exit 1
fi
echo "qemu-prod64: PASS"
