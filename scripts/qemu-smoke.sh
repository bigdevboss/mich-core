#!/bin/sh
set -eu

image="${1:-bin/disk.img}"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT

set +e
timeout 8s qemu-system-i386 \
    -drive file="$image",format=raw,if=ide,index=0,media=disk \
    -boot c \
    -m 128M \
    -serial stdio \
    -display none \
    -no-reboot \
    -no-shutdown >"$log" 2>&1
status=$?
set -e

if [ "$status" -ne 0 ] && [ "$status" -ne 124 ]; then
    cat "$log"
    echo "qemu-smoke: QEMU exited unexpectedly (status $status)" >&2
    exit 1
fi

for marker in \
    "s2: jumping to kernel" \
    "Mich: paging enabled" \
    "Mich: PMM done" \
    "Mich: tasks ready, enabling preemption" \
    "[PASS] service register" \
    "[PASS] service lookup" \
    "[PASS] boot capabilities received" \
    "[PASS] bad user pointer rejected" \
    "[PASS] copy-on-write address isolation" \
    "[PASS] copy-on-write single owner fast path" \
    "[PASS] fork" \
    "[PASS] wait and exit status" \
    "[PASS] blocking IPC delivery" \
    "[PASS] IPC sender resumed" \
    "[PASS] IPC deadlock cycle rejected" \
    "[PASS] IPC send timeout" \
    "[PASS] nonblocking IPC ready receiver" \
    "[PASS] blocked sender wakes when receiver dies" \
    "[PASS] receiver notified when sender dies" \
    "[PASS] user page fault contained" \
    "[PASS] unprivileged service register denied" \
    "[PASS] capability grant and drop" \
    "[PASS] capability-owned service released" \
    "[PASS] ungranted IRQ denied" \
    "[PASS] object IRQ grant" \
    "[PASS] object I/O port grant" \
    "[PASS] RAM cannot be granted as MMIO" \
    "[PASS] object MMIO grant and map" \
    "[PASS] bounded contiguous DMA buffer" \
    "[PASS] DMA pages reclaimed on exit" \
    "[PASS] user invalid opcode contained" \
    "[PASS] child cannot kill ancestor" \
    "[PASS] process can manage descendants" \
    "[PASS] orphan adopted by init" \
    "Mich init: ALL TESTS PASSED"
do
    if ! grep -Fq "$marker" "$log"; then
        cat "$log"
        echo "qemu-smoke: missing marker: $marker" >&2
        exit 1
    fi
done

if grep -Eq "PANIC|DOUBLE FAULT|s1: FAIL|s2: .*FAIL|\[FAIL\]|TESTS FAILED" "$log"; then
    cat "$log"
    echo "qemu-smoke: kernel or bootloader failure detected" >&2
    exit 1
fi

cat "$log"
echo "qemu-smoke: PASS"
