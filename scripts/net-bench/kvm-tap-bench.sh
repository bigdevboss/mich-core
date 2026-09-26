#!/bin/sh
# Runs the netbench wire pass over a real tap + vhost-net topology on a KVM box,
# so the bulk-throughput, round-trip, and packet-rate numbers reflect the guest
# stack instead of QEMU slirp. slirp reimplements TCP in QEMU userspace and caps
# the wire path around a few Mbit/s; tap hands frames straight to the host kernel
# and vhost-net moves the virtio datapath out of QEMU into a host kernel thread.
#
# This has to run as root: creating a tap device, giving it an address, and
# serving DHCP on it are all privileged. It sets everything up, builds the heavy
# netbench capsule, runs it under KVM, and tears the tap and DHCP server back down
# on exit. The build artifacts are chowned back to the invoking user so the git
# tree does not end up root-owned.
#
# Usage:
#   sudo scripts/net-bench/kvm-tap-bench.sh
#
# Overridable with environment variables:
#   TAP_IF   tap interface name          (default tap0)
#   HOST_IP  address on the host side     (default 10.0.2.4, the peer address)
#   GUEST_IP address leased to the guest  (default 10.0.2.15)
set -eu

TAP_IF="${TAP_IF:-tap0}"
HOST_IP="${HOST_IP:-10.0.2.4}"
GUEST_IP="${GUEST_IP:-10.0.2.15}"

# The peer, the guest capsule, and slirp all agree on 10.0.2.x; keep the tap on
# the same /24 so none of the hardcoded addresses need to change.
NETMASK="255.255.255.0"

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
tap_owner="${SUDO_USER:-root}"
dnsmasq_pid=""

fail() { echo "kvm-tap-bench: $1" >&2; exit 1; }

[ "$(id -u)" = "0" ] || fail "must run as root (use sudo)"

# Fail early with an actionable message rather than deep inside QEMU or dnsmasq.
command -v qemu-system-x86_64 >/dev/null 2>&1 || fail "qemu-system-x86_64 not found"
command -v dnsmasq >/dev/null 2>&1 || fail "dnsmasq not found (pacman -S dnsmasq)"
command -v ip >/dev/null 2>&1 || fail "ip not found (pacman -S iproute2)"
[ -c /dev/vhost-net ] || fail "/dev/vhost-net missing (modprobe vhost_net)"
[ -c /dev/kvm ] || fail "/dev/kvm missing (KVM not available)"

cleanup() {
    [ -n "$dnsmasq_pid" ] && kill "$dnsmasq_pid" 2>/dev/null || true
    ip link show "$TAP_IF" >/dev/null 2>&1 && ip link del "$TAP_IF" 2>/dev/null || true
    # Undo the root ownership the in-place build would otherwise leave behind.
    [ "$tap_owner" != "root" ] && chown -R "$tap_owner" "$repo_root/bin" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# A stale tap from an aborted run would make `ip tuntap add` fail; clear it first.
ip link show "$TAP_IF" >/dev/null 2>&1 && ip link del "$TAP_IF"
ip tuntap add dev "$TAP_IF" mode tap user "$tap_owner"
ip addr add "$HOST_IP/24" dev "$TAP_IF"
ip link set "$TAP_IF" up

# DHCP only: --port=0 turns off the dnsmasq DNS server so it never fights
# systemd-resolved for port 53, and a single-address range hands the guest the
# fixed lease the capsule expects. --bind-interfaces plus --except-interface keep
# it off every other interface on the box.
dnsmasq \
    --interface="$TAP_IF" \
    --bind-interfaces \
    --except-interface=lo \
    --port=0 \
    --dhcp-range="$GUEST_IP,$GUEST_IP,$NETMASK,1h" \
    --dhcp-authoritative \
    --no-resolv \
    --no-hosts \
    --pid-file="/run/kvm-tap-bench-dnsmasq.pid" &
dnsmasq_pid=$!

echo "kvm-tap-bench: tap=$TAP_IF host=$HOST_IP guest=$GUEST_IP, building heavy netbench capsule"

# Rebuild clean: make does not track the -DMICH_NETBENCH_TAP flag change, so a
# stale object from a slirp build would silently keep the tiny sizes.
make -C "$repo_root" clean >/dev/null
MICH_KVM=1 MICH_NET_TAP=1 MICH_TAP_IF="$TAP_IF" make -C "$repo_root" test64-netbench

echo "kvm-tap-bench: done"
