#!/bin/sh
set -eu
image="${1:-bin/x86_64/disk-unit.img}"
. "$(dirname "$0")/uefi-firmware.sh"
mich_uefi_firmware
log="$(mktemp)"
trap 'rm -f "$log" "$uefi_vars"' EXIT
set +e
timeout 45s qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64,+aes,+pclmulqdq,+ssse3 \
    -drive if=pflash,format=raw,readonly=on,file="$uefi_code" \
    -drive if=pflash,format=raw,file="$uefi_vars" \
    -drive file="$image",format=raw,if=none,id=esdisk \
    -device ide-hd,drive=esdisk,bootindex=1 \
    -m 256M \
    -serial stdio \
    -display none \
    -no-reboot \
    -device isa-debug-exit >"$log" 2>&1
status=$?
set -e
if [ "$status" -ne 0 ] && [ "$status" -ne 33 ]; then
    cat "$log"
    echo "qemu-unit64: FAIL (status $status)" >&2
    exit 1
fi
echo "qemu-unit64: PASS (status $status)"
