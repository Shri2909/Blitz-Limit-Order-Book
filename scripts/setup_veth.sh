#!/usr/bin/env bash
#
# scripts/setup_veth.sh
#
# HYDRA-LOB Phase 10 -- dev-box AF_XDP test harness.
#
# There's no real NIC/driver pair available to validate XDP_ZEROCOPY on, so
# this creates a veth pair instead. veth only supports AF_XDP's XDP_COPY
# mode and generic (SKB) program attachment -- see xdp_socket.hpp's own WHY
# comment -- so this validates ring-plumbing/parsing correctness, NOT
# zero-copy performance. Never quote latency numbers measured this way.
#
# Usage:
#   sudo ./scripts/setup_veth.sh up      # create veth-hydra0 / veth-hydra1
#   sudo ./scripts/setup_veth.sh down    # remove them
#
# Topology: veth-hydra0 stays in the root/default netns (this is what
# blitz_lob --xdp-iface binds to); veth-hydra1 is moved into its own netns
# ("hydra-ns"). WHY the namespace split (an earlier version of this script
# put both ends in the same netns with addresses on the same /24 -- don't
# reintroduce that): with both ends visible in one routing table, the
# kernel can resolve a locally-originated packet to 10.200.0.1 as
# on-link-via-veth-hydra0 without ever putting it on the wire, so it never
# reaches veth-hydra0's XDP ingress hook at all -- it falls through to the
# ordinary IP stack, finds nothing bound to the UDP port, and the sender
# sees ICMP port-unreachable ("Connection refused") even though the XDP
# program and AF_XDP socket are both working correctly. Forcing the sender
# into a separate netns removes the ambiguity: the only way out of
# hydra-ns is across the veth pair.

set -euo pipefail

IFACE0="veth-hydra0"
IFACE1="veth-hydra1"
IFACE0_ADDR="10.200.0.1/24"
IFACE1_ADDR="10.200.0.2/24"
NETNS="hydra-ns"

if [[ "${EUID}" -ne 0 ]]; then
    echo "error: must run as root (creates network interfaces/namespaces)" >&2
    exit 1
fi

usage() {
    echo "Usage: $0 [up|down]" >&2
    exit 1
}

cmd_down() {
    ip link del "$IFACE0" 2>/dev/null || true
    ip netns del "$NETNS" 2>/dev/null || true
    echo "setup_veth.sh: removed $IFACE0/$IFACE1 and netns $NETNS (if present)"
}

cmd_up() {
    cmd_down

    ip netns add "$NETNS"

    ip link add "$IFACE0" type veth peer name "$IFACE1"
    ip link set "$IFACE1" netns "$NETNS"

    ip addr add "$IFACE0_ADDR" dev "$IFACE0"
    ip netns exec "$NETNS" ip addr add "$IFACE1_ADDR" dev "$IFACE1"

    # AF_XDP-on-veth prerequisite: GRO/GSO/TSO/checksum offload on a veth
    # pair can hand XDP a frame shape (aggregated, or missing checksums)
    # net/xdp_prog.bpf.c and parse_order_zero_copy() don't expect -- turn
    # it all off on both ends so packets arrive as plain, individual
    # Ethernet+IPv4+UDP frames.
    ethtool -K "$IFACE0" tso off gso off gro off rx off tx off 2>/dev/null || true
    ip netns exec "$NETNS" ethtool -K "$IFACE1" tso off gso off gro off rx off tx off 2>/dev/null || true

    ip link set "$IFACE0" up
    ip netns exec "$NETNS" ip link set "$IFACE1" up
    ip netns exec "$NETNS" ip link set lo up

    echo "setup_veth.sh: $IFACE0 ($IFACE0_ADDR, root netns) <-> $IFACE1 ($IFACE1_ADDR, netns $NETNS) up"
    echo ""
    echo "Next steps:"
    echo "  Terminal A (root netns): sudo ./build/blitz_lob --xdp-iface $IFACE0 --xdp-queue 0 \\"
    echo "                  --xdp-prog net/xdp_prog.o --xdp-attach-mode skb"
    echo "  Terminal B (inside $NETNS, note ip netns exec): sudo ip netns exec $NETNS \\"
    echo "                  ./build/blitz_send_test_orders \\"
    echo "                  --target 10.200.0.1:40000 --count 2000 --rate-hz 500"
    echo ""
    echo "Teardown: sudo $0 down"
}

case "${1:-}" in
    up) cmd_up ;;
    down) cmd_down ;;
    *) usage ;;
esac
