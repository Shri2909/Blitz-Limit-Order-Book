/* SPDX-License-Identifier: GPL-2.0 */
/*
 * net/xdp_prog.bpf.c
 *
 * HYDRA-LOB Phase 10 -- AF_XDP redirect program.
 *
 * Redirects UDP traffic (the order-entry wire protocol, see
 * include/hydra/xdp/zero_copy_parser.hpp's OrderWireFormat) on the bound
 * {netdev, queue} pair into xsks_map, so a hydra::xdp::XdpSocket bound to
 * the same queue receives it via zero-copy AF_XDP. Everything else (ARP,
 * TCP, ICMP, non-order UDP, ...) falls through to XDP_PASS, so the
 * interface keeps behaving like a normal NIC for everything that isn't
 * order-entry traffic.
 *
 * Uses parsing_helpers.h, vendored from the xdp-tutorial project, for
 * bounds-checked Ethernet/IPv4 header parsing instead of hand-rolling
 * pointer arithmetic here.
 *
 * Compiled separately from the C++ build (see net/Makefile) -- BPF object
 * code is produced by clang -target bpf, not the C++ toolchain, and gets
 * loaded into the kernel at runtime via
 * hydra::xdp::load_and_attach_xdp_program() (net/xdp_socket_user.cpp), not
 * linked into the blitz_lob binary:
 *   clang -O2 -target bpf -c net/xdp_prog.bpf.c -o net/xdp_prog.o
 */
#include <linux/bpf.h>
#include <linux/in.h> /* IPPROTO_UDP */

#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#include "parsing_helpers.h"

/* Must track hydra::config::AFXDP_ORDER_ENTRY_UDP_PORT (include/hydra/config.hpp).
 * Duplicated here (not shared via a common header) because this file is
 * compiled by a separate clang -target bpf toolchain (see net/Makefile) and
 * is deliberately self-contained, matching parsing_helpers.h's own
 * vendored/no-cross-repo-dependency policy -- if you change one, change
 * both. */
#define HYDRA_ORDER_ENTRY_UDP_PORT 40000

/* IP_MF / IP_OFFMASK are userspace (netinet/ip.h) macros, not available
 * from the kernel uapi headers this BPF program is restricted to -- defined
 * locally with their standard, stable values (RFC 791: bit 13 = more-
 * fragments flag, bits 0-12 = 13-bit fragment offset in 8-byte units). */
#define HYDRA_IP_MF      0x2000
#define HYDRA_IP_OFFMASK 0x1FFF

/* One entry per NIC queue this program can be attached to. A queue's entry
 * is populated (by xsk_socket__update_xskmap(), see hydra::xdp::XdpSocket's
 * constructor in net/xdp_socket_user.cpp) only once an AF_XDP socket has
 * actually bound to that queue -- bpf_map_lookup_elem() returning NULL
 * below means "no HYDRA-LOB socket listening here yet", not an error. */
struct
{
	__uint(type, BPF_MAP_TYPE_XSKMAP);
	__type(key, __u32);
	__type(value, __u32);
	__uint(max_entries, 64);
} xsks_map SEC(".maps");

/* Per-CPU redirect/pass/drop counters. WHY: a preflight check should be
 * able to confirm packets are actually reaching the intended queue and
 * being classified as UDP before blaming silence on the C++ pipeline
 * (SPSC queue empty, matcher stalled, etc.) further downstream. */
struct
{
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__type(key, __u32);
	__type(value, __u64);
	__uint(max_entries, 5);
} xdp_prog_stats SEC(".maps");

enum hydra_stat_slot
{
	HYDRA_STAT_REDIRECTED  = 0,
	HYDRA_STAT_PASSED      = 1,
	HYDRA_STAT_NOT_UDP     = 2,
	HYDRA_STAT_WRONG_PORT  = 3,
	HYDRA_STAT_FRAGMENTED  = 4,
};

static __always_inline void bump_stat(__u32 slot)
{
	__u64 *counter = bpf_map_lookup_elem(&xdp_prog_stats, &slot);

	if (counter)
		(*counter)++;
}

SEC("xdp")
int hydra_xdp_redirect(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data     = (void *)(long)ctx->data;
	struct hdr_cursor nh = { .pos = data };
	struct ethhdr *eth;
	struct iphdr *iph;
	int eth_type, ip_type;
	__u32 queue_index = ctx->rx_queue_index;

	eth_type = parse_ethhdr(&nh, data_end, &eth);
	if (eth_type != bpf_htons(ETH_P_IP)) {
		bump_stat(HYDRA_STAT_NOT_UDP);
		return XDP_PASS;
	}

	ip_type = parse_iphdr(&nh, data_end, &iph);
	if (ip_type != IPPROTO_UDP) {
		bump_stat(HYDRA_STAT_NOT_UDP);
		return XDP_PASS;
	}

	/* Reject fragments before trusting anything past the IP header: a
	 * non-initial fragment has ip->protocol == IPPROTO_UDP but no UDP
	 * header at this offset at all (it's raw payload continuation
	 * bytes), which parse_udphdr() below has no way to detect on its
	 * own -- it would just read garbage as if it were a udphdr. The
	 * order-entry wire protocol (72 bytes total, see
	 * zero_copy_parser.hpp's OrderWireFormat) never legitimately
	 * fragments, so any fragment here is by definition not real order
	 * traffic. bpf_ntohs() first: frag_off is network-byte-order. */
	if (bpf_ntohs(iph->frag_off) & (HYDRA_IP_MF | HYDRA_IP_OFFMASK)) {
		bump_stat(HYDRA_STAT_FRAGMENTED);
		return XDP_PASS;
	}

	/* Only redirect the order-entry port -- everything else (DNS, mDNS,
	 * any other UDP service sharing this queue) must keep flowing
	 * through the normal stack, not compete for this socket's UMEM/ring
	 * capacity or reach parse_order_zero_copy() (zero_copy_parser.hpp),
	 * which has no checksum/magic/version validation of its own and
	 * will happily parse any >=30-byte UDP payload as if it were a real
	 * Order. WHY 40000 duplicated instead of shared: see
	 * HYDRA_ORDER_ENTRY_UDP_PORT's definition above. */
	{
		struct udphdr *udph;
		int udp_len = parse_udphdr(&nh, data_end, &udph);

		if (udp_len < 0 || bpf_ntohs(udph->dest) != HYDRA_ORDER_ENTRY_UDP_PORT) {
			bump_stat(HYDRA_STAT_WRONG_PORT);
			return XDP_PASS;
		}
	}

	/* Only redirect if a HYDRA-LOB AF_XDP socket is actually bound to
	 * this queue. Redirecting into an empty xsks_map slot would make
	 * bpf_redirect_map() fail; passing here instead keeps ordinary
	 * kernel networking working on this queue until a socket binds. */
	if (!bpf_map_lookup_elem(&xsks_map, &queue_index)) {
		bump_stat(HYDRA_STAT_PASSED);
		return XDP_PASS;
	}

	bump_stat(HYDRA_STAT_REDIRECTED);
	return bpf_redirect_map(&xsks_map, queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";
