#!/bin/sh
set -eu
image="${1:-bin/x86_64/disk-unit.img}"
. "$(dirname "$0")/uefi-firmware.sh"
mich_uefi_firmware
# The smoke runner grew MICH_QEMU_TIMEOUT for hosts slower than the developer
# box; a shared CI runner needs the same escape hatch here.
qemu_timeout="${MICH_QEMU_TIMEOUT:-45}"
log="$(mktemp)"
trap 'rm -f "$log" "$uefi_vars"' EXIT
set +e
timeout "$qemu_timeout" qemu-system-x86_64 \
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
