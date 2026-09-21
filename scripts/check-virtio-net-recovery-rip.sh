#!/bin/sh
set -eu

kernel_source=${1:?kernel source required}
capsule=${2:?capsule required}
objdump=${OBJDUMP:-objdump}

expected=$(sed -n 's/^#define[[:space:]][[:space:]]*VIRTIO_NET_RECOVERY_TEST_RIP[[:space:]][[:space:]]*\(0x[0-9A-Fa-f][0-9A-Fa-f]*\)ULL$/\1/p' "$kernel_source")
[ -n "$expected" ] || {
    echo "virtio recovery RIP check: expected selector literal missing" >&2
    exit 1
}
[ "$(printf '%s\n' "$expected" | wc -l | tr -d '[:space:]')" -eq 1 ] || {
    echo "virtio recovery RIP check: expected selector literal ambiguous" >&2
    exit 1
}

actual=$("$objdump" -d "$capsule" | awk '
$2 == "0f" && $3 == "0b" {
    address = $1
    sub(/:$/, "", address)
    print "0x" address
}')
[ -n "$actual" ] || {
    echo "virtio recovery RIP check: primary capsule has no ud2" >&2
    exit 1
}
[ "$(printf '%s\n' "$actual" | wc -l | tr -d '[:space:]')" -eq 1 ] || {
    echo "virtio recovery RIP check: primary capsule has multiple ud2 instructions" >&2
    exit 1
}
[ "$expected" = "$actual" ] || {
    printf 'virtio recovery RIP check: expected %s, capsule ud2 is %s\n' \
        "$expected" "$actual" >&2
    exit 1
}
