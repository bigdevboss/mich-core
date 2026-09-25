#!/bin/sh
set -eu
image="${1:-bin/x86_64/disk.img}"
. "$(dirname "$0")/uefi-firmware.sh"
mich_uefi_firmware
trap 'rm -f "$uefi_vars"' EXIT
exec qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64,+aes,+pclmulqdq,+ssse3 \
    -drive if=pflash,format=raw,readonly=on,file="$uefi_code" \
    -drive if=pflash,format=raw,file="$uefi_vars" \
    -drive file="$image",format=raw,if=none,id=esdisk \
    -device ide-hd,drive=esdisk,bootindex=1 \
    -m 128M \
    -serial stdio \
    -vga std \
    -display none \
    -no-reboot \
    -nic none
