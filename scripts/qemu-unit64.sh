#!/bin/sh
set -eu
image="${1:-bin/x86_64/disk-unit.img}"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT
set +e
timeout 20s qemu-system-x86_64 \
    -drive file="$image",format=raw,if=ide,index=0,media=disk \
    -boot c \
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
