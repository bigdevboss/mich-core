#!/bin/sh
set -eu

# Crash harness for the adytumfs generation pair. Boots the crash workload
# image against a persistent NVMe drive, cuts power at chosen serial points,
# and requires every next boot to mount whatever the crash left behind. The
# first four cuts hit the commit windows the in-battery fault injector covers
# deterministically; the cuts after that land at arbitrary moments. The last
# boot must run the whole workload to its end marker.

image=${1:?missing image}
memory=${2:?missing memory}
runs=${3:-7}

case "$runs" in
    ''|*[!0-9]*)
        echo "qemu-crash64: invalid run count: $runs" >&2
        exit 1
        ;;
esac
if [ "$runs" -lt 3 ]; then
    echo "qemu-crash64: run count must leave room for the plan and a final boot" >&2
    exit 1
fi

accel_cpu="-cpu qemu64,+aes,+pclmulqdq,+ssse3"
if [ -n "${MICH_KVM:-}" ]; then accel_cpu="-enable-kvm -cpu host"; fi
qemu_timeout="${MICH_QEMU_TIMEOUT:-240}"
# cache.direct pushes every guest write straight to the image file, so a cut
# lands exactly the bytes the guest issued. It needs a filesystem with
# O_DIRECT; without it QEMU loses whole acknowledged writes on a cut, which
# only ever leaves an older committed generation behind.
nvme_cache=""
if [ -n "${MICH_CRASH_DIRECT:-}" ]; then nvme_cache=",cache.direct=on"; fi

workdir="$(mktemp -d)"
nvme_img="$workdir/crash-nvme.img"
blk_img="$workdir/crash-blk.img"
log="$workdir/crash.log"
uefi_vars=""
trap 'if [ -n "$uefi_vars" ] && [ -f "$uefi_vars" ]; then rm -f "$uefi_vars"; fi; rm -rf "$workdir"' EXIT

dd if=/dev/zero of="$nvme_img" bs=1M count=16 2>/dev/null
dd if=/dev/zero of="$blk_img" bs=512 count=256 2>/dev/null
. "$(dirname "$0")/uefi-firmware.sh"
mich_uefi_firmware

# The kill plan for one cut: the first marker opens the window before the
# commit lands, the second one right after the flip; the rest cut wherever
# the boot happens to be.
plan_for() {
    case $1 in
        1) echo "marker:1:point .* open" ;;
        2) echo "marker:4:point .* committed" ;;
        3) echo "marker:7:point .* open" ;;
        4) echo "marker:10:point .* committed" ;;
        5) echo "sleep:3" ;;
        6) echo "sleep:1" ;;
        *) echo "sleep:2" ;;
    esac
}

# What the boot after each cut must report. The cut lands within one round
# of the marker, so the expected count carries the same slack.
expect_for() {
    case $1 in
        2) echo "0 1" ;;
        3) echo "4 5" ;;
        4) echo "6 7" ;;
        5) echo "10 11" ;;
        *) echo "" ;;
    esac
}

die_with_log() {
    echo "qemu-crash64: $1" >&2
    cat "$log" 2>/dev/null || true
    cat "$workdir/qemu.err" 2>/dev/null || true
    exit 1
}

require_marker() {
    if ! grep -q "$1" "$log" 2>/dev/null; then
        die_with_log "missing marker: $1"
    fi
}

refuse_panic() {
    if grep -q "MICH KERNEL PANIC" "$log" 2>/dev/null; then
        die_with_log "kernel panic in boot $1"
    fi
}

qemu_pid=""

# One boot against the serial log. The plan says how to end it: cut at the
# count-th match of a marker regex, cut after a sleep, or let the workload
# run to its end and stop the parked machine.
boot_once() {
    plan=$1
    rm -f "$log"
    rm -f "$uefi_vars"
    mich_uefi_firmware
    qemu-system-x86_64 \
        -machine q35 \
        $accel_cpu \
        -drive if=pflash,format=raw,readonly=on,file="$uefi_code" \
        -drive if=pflash,format=raw,file="$uefi_vars" \
        -drive file="$image",format=raw,if=none,id=esdisk \
        -device ide-hd,drive=esdisk,bootindex=1 \
        -drive file="$blk_img",format=raw,if=none,id=michblk \
        -device virtio-blk-pci,drive=michblk,disable-legacy=on \
        -drive file="$nvme_img",format=raw,if=none,id=michnvme$nvme_cache \
        -device nvme,drive=michnvme,serial=michx0 \
        -m "$memory" \
        -serial file:"$log" \
        -vga std \
        -display none \
        -no-reboot \
        -no-shutdown >"$workdir/qemu.err" 2>&1 &
    qemu_pid=$!
    deadline=$(( $(date +%s) + qemu_timeout ))
    if [ "$plan" = none ]; then
        while :; do
            if grep -q "workload complete" "$log" 2>/dev/null; then
                kill "$qemu_pid" 2>/dev/null || true
                break
            fi
            if ! kill -0 "$qemu_pid" 2>/dev/null; then break; fi
            if [ "$(date +%s)" -ge "$deadline" ]; then
                kill -9 "$qemu_pid" 2>/dev/null || true
                die_with_log "final boot timed out"
            fi
            sleep 1
        done
    elif [ "${plan%%:*}" = marker ]; then
        want=$(echo "$plan" | cut -d: -f2)
        regex=$(echo "$plan" | cut -d: -f3)
        while :; do
            seen=$(grep -c "$regex" "$log" 2>/dev/null || true)
            if [ "${seen:-0}" -ge "$want" ]; then
                kill -9 "$qemu_pid" 2>/dev/null || true
                break
            fi
            if ! kill -0 "$qemu_pid" 2>/dev/null; then
                die_with_log "qemu exited before cut point $plan"
            fi
            if [ "$(date +%s)" -ge "$deadline" ]; then
                kill -9 "$qemu_pid" 2>/dev/null || true
                die_with_log "boot timed out before cut point $plan"
            fi
            sleep 0.05
        done
    else
        delay=$(echo "$plan" | cut -d: -f2)
        sleep "$delay"
        kill -9 "$qemu_pid" 2>/dev/null || true
    fi
    wait "$qemu_pid" 2>/dev/null || true
}

# Check the recovery report of a boot that follows a cut. The expectation is
# the range of committed rounds the cut can leave behind; empty means the
# cut landed wherever it landed and any clean recovery is the assertion.
check_recovery() {
    boot=$1
    range=$(expect_for "$boot")
    require_marker "Mich crash: recovery rounds"
    if [ -z "$range" ]; then
        echo "qemu-crash64: PASS (recovery after cut $((boot - 1)), any round)"
        return
    fi
    low=$(echo "$range" | cut -d' ' -f1)
    high=$(echo "$range" | cut -d' ' -f2)
    found=$(sed -n 's/^Mich crash: recovery rounds \([0-9A-Fa-f]*\)$/\1/p' "$log")
    if [ -z "$found" ]; then
        die_with_log "recovery report did not parse in boot $boot"
    fi
    value=$(printf '%d' "0x$found")
    if [ "$value" -lt "$low" ] || [ "$value" -gt "$high" ]; then
        die_with_log "boot $boot recovered rounds $value, expected $low..$high"
    fi
    echo "qemu-crash64: PASS (recovery after cut $((boot - 1)), rounds $value)"
}

# A sleep cut can land before the boot reaches the workload, in which case
# the cut wrote nothing and the volume carries the state of the boot before
# it: only a boot whose log reached the workload has a recovery to judge.
run=1
while [ "$run" -lt "$runs" ]; do
    boot_once "$(plan_for "$run")"
    refuse_panic "$run"
    if grep -q "Mich crash:" "$log" 2>/dev/null; then
        if [ "$run" -eq 1 ]; then
            require_marker "Mich crash: volume formatted"
        else
            check_recovery "$run"
        fi
    else
        echo "qemu-crash64: PASS (cut $run/$((runs - 1)) before the workload)"
    fi
    echo "qemu-crash64: PASS (cut $run/$((runs - 1)))"
    run=$((run + 1))
done

boot_once none
check_recovery "$runs"
require_marker "Mich crash: workload complete"
refuse_panic "$runs"
echo "qemu-crash64: PASS ($runs boots)"
