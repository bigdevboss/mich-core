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
#   MICH_TAP_TCPDUMP=1  capture tap TCP to netbench-tap.txt for wire debugging
#   MICH_TAP_PCAP=path  override where that capture is written
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
tcpdump_pid=""
# Where the opt-in wire capture lands. A bulk run is tens of thousands of
# packets, so it must stream to a file rather than the terminal; the tail of it
# is what shows the stall.
tcpdump_out="${MICH_TAP_PCAP:-$repo_root/netbench-tap.txt}"

# Opt-in wire capture. When the wire pass fails there is no way to tell a dropped
# handshake (firewall, bad checksum) from a stack bug without seeing the frames,
# so MICH_TAP_TCPDUMP=1 snapshots the tap while the run happens. -v prints the
# checksum verdict per segment, which is what separates a guest checksum bug from
# a host-side drop.
MICH_TAP_TCPDUMP="${MICH_TAP_TCPDUMP:-0}"

fail() { echo "kvm-tap-bench: $1" >&2; exit 1; }

[ "$(id -u)" = "0" ] || fail "must run as root (use sudo)"

# Fail early with an actionable message rather than deep inside QEMU or dnsmasq.
command -v qemu-system-x86_64 >/dev/null 2>&1 || fail "qemu-system-x86_64 not found"
command -v dnsmasq >/dev/null 2>&1 || fail "dnsmasq not found (pacman -S dnsmasq)"
command -v ip >/dev/null 2>&1 || fail "ip not found (pacman -S iproute2)"
[ -c /dev/vhost-net ] || fail "/dev/vhost-net missing (modprobe vhost_net)"
[ -c /dev/kvm ] || fail "/dev/kvm missing (KVM not available)"

cleanup() {
    [ -n "$tcpdump_pid" ] && kill "$tcpdump_pid" 2>/dev/null || true
    [ -n "$dnsmasq_pid" ] && kill "$dnsmasq_pid" 2>/dev/null || true
    ip link show "$TAP_IF" >/dev/null 2>&1 && ip link del "$TAP_IF" 2>/dev/null || true
    # Undo the root ownership the in-place build would otherwise leave behind.
    [ "$tap_owner" != "root" ] && chown -R "$tap_owner" "$repo_root/bin" 2>/dev/null || true
    # The capture is written as root; hand it back so it can be read without sudo.
    [ "$tap_owner" != "root" ] && [ -f "$tcpdump_out" ] && \
        chown "$tap_owner" "$tcpdump_out" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# A stale tap from an aborted run would make `ip tuntap add` fail; clear it first.
ip link show "$TAP_IF" >/dev/null 2>&1 && ip link del "$TAP_IF"
ip tuntap add dev "$TAP_IF" mode tap user "$tap_owner"
ip addr add "$HOST_IP/24" dev "$TAP_IF"
ip link set "$TAP_IF" up

# Start the capture before DHCP so the log shows the whole conversation: the DHCP
# handshake, the ARP for the peer, the ICMP that already works, and the TCP SYN
# whose fate is the open question. Stream TCP to a file with sequence and ack
# numbers (-S absolute, so a retransmit or a run of duplicate acks is obvious
# across the whole transfer) and no packet cap: the earlier -c 200 stopped during
# the handshake, long before a multi-megabyte bulk stall, so it never caught the
# failure it was meant to explain. -l keeps it line buffered so a killed run still
# leaves a complete tail on disk.
if [ "$MICH_TAP_TCPDUMP" = "1" ]; then
    command -v tcpdump >/dev/null 2>&1 || fail "tcpdump not found (pacman -S tcpdump)"
    tcpdump -i "$TAP_IF" -n -S -l 'tcp' >"$tcpdump_out" 2>/dev/null &
    tcpdump_pid=$!
    echo "kvm-tap-bench: wire capture -> $tcpdump_out"
fi

# DHCP only: --port=0 turns off the dnsmasq DNS server so it never fights
# systemd-resolved for port 53, and a single-address range hands the guest the
# fixed lease the capsule expects. --bind-interfaces plus --except-interface keep
# it off every other interface on the box. The guest capsule rejects any offer
# that lacks a router (option 3), a netmask, and a server id, so the router is set
# explicitly to the host tap address rather than trusting a default. --no-daemon
# keeps the pid trackable so cleanup can stop it (a daemonized dnsmasq would leak
# past a deleted tap), and --log-dhcp writes every DHCP transaction into the run
# log so a lease failure is visible instead of silent.
dnsmasq \
    --no-daemon \
    --log-dhcp \
    --log-facility=- \
    --interface="$TAP_IF" \
    --bind-interfaces \
    --except-interface=lo \
    --port=0 \
    --dhcp-range="$GUEST_IP,$GUEST_IP,$NETMASK,1h" \
    --dhcp-option=3,"$HOST_IP" \
    --dhcp-authoritative \
    --no-resolv \
    --no-hosts &
dnsmasq_pid=$!

# A backgrounded dnsmasq that dies at startup (a port clash, a bad option) would
# otherwise leave the guest with no lease and only a silent wire failure to show
# for it, so confirm it is actually serving before spending minutes on the build.
sleep 1
kill -0 "$dnsmasq_pid" 2>/dev/null || fail "dnsmasq exited at startup (see its log above)"

echo "kvm-tap-bench: tap=$TAP_IF host=$HOST_IP guest=$GUEST_IP, building heavy netbench capsule"

# Rebuild clean: make does not track the -DMICH_NETBENCH_TAP flag change, so a
# stale object from a slirp build would silently keep the tiny sizes.
make -C "$repo_root" clean >/dev/null
MICH_KVM=1 MICH_NET_TAP=1 MICH_TAP_IF="$TAP_IF" make -C "$repo_root" test64-netbench

echo "kvm-tap-bench: done"
