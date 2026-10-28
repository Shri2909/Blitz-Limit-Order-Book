#pragma once

#ifdef ENABLE_AFXDP

// include/hydra/xdp/zero_copy_parser.hpp
//
// HYDRA-LOB Phase 10 -- zero-copy Order parser.
//
// Parses hydra::Order fields directly out of a UMEM frame pointer -- zero
// memcpy on this path. Every field is read via reinterpret_cast/offset
// access into the mapped frame memory; nothing is staged through an
// intermediate buffer first. Pairs with hydra::xdp::XdpSocket
// (xdp_socket.hpp): recv_batch() hands out FrameDescriptors, this header
// turns them into Orders, and release_frame() returns the UMEM slot to the
// kernel for reuse.
//
// This entire file compiles to nothing when ENABLE_AFXDP is undefined.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <arpa/inet.h>
#include <endian.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>

#include "hydra/types.hpp"
#include "hydra/xdp/xdp_socket.hpp"

namespace hydra::xdp
{

    // On-the-wire layout of one order-entry message, carried as the
    // payload of a UDP datagram. Fixed-width, network-byte-order, tightly
    // packed so the parser can read it straight off the UMEM frame with a
    // single reinterpret_cast -- no per-field marshalling struct, no
    // memcpy.
    //
    // WHY this is a separate struct from hydra::Order (types.hpp): Order
    // is cache-line padded and hot/cold split for the matching path (see
    // types.hpp's WHY comment), which is the opposite of what a compact
    // wire format wants. Keeping them distinct also means the wire
    // protocol can evolve (e.g. add a version byte) without touching the
    // 128-byte hot-path layout the matcher depends on.
    //
    // WHY `tif` is carried on the wire (unlike a bare BUY/SELL-only
    // protocol): config.hpp's DEFAULT_IOC_RATIO/DEFAULT_FOK_RATIO already
    // assume incoming order flow includes IOC/FOK orders, not just GTC --
    // an order-entry protocol that couldn't express TimeInForce would make
    // those two constants unreachable from the real AF_XDP RX path.
#pragma pack(push, 1)
    struct OrderWireFormat
    {
        uint64_t order_id;
        int64_t price;
        uint32_t qty;
        uint64_t client_id;
        uint8_t side; // 0 = hydra::Side::BUY, 1 = hydra::Side::SELL
        uint8_t tif;  // 0 = GTC, 1 = IOC, 2 = FOK -- matches hydra::TimeInForce's underlying values
    };
#pragma pack(pop)
    static_assert(sizeof(OrderWireFormat) == 30,
                  "OrderWireFormat layout changed -- bump a wire version "
                  "byte before changing this, callers/generators may be "
                  "pinned to the old layout");

    // A single 802.1Q/802.1AD VLAN tag (4 bytes: priority+VLAN-ID, then the
    // encapsulated EtherType). WHY this exists here: the eBPF classifier
    // (net/xdp_prog.bpf.c, via parsing_helpers.h's parse_ethhdr_vlan())
    // already skips VLAN tags when deciding whether to redirect a frame --
    // without a matching skip here, a VLAN-tagged order-entry packet would
    // be correctly redirected by the kernel side and then incorrectly
    // rejected as a parse_error here, since eth->h_proto would read as
    // ETH_P_8021Q/AD instead of ETH_P_IP.
#pragma pack(push, 1)
    struct VlanTag
    {
        uint16_t tci; // priority + VLAN ID; not needed for order parsing
        uint16_t encapsulated_proto; // inner EtherType, network byte order
    };
#pragma pack(pop)
    static_assert(sizeof(VlanTag) == 4, "VlanTag must be exactly 4 bytes");

    namespace detail
    {

        [[nodiscard]] inline int64_t ntoh64(uint64_t v) noexcept
        {
            return static_cast<int64_t>(be64toh(v));
        }

    } // namespace detail

    // Parses an Order out of a raw UMEM frame (Ethernet + IPv4 + UDP +
    // OrderWireFormat) with zero memcpy. Returns false -- leaving `out`
    // untouched -- if the frame is too short, isn't IPv4/UDP, or the UDP
    // payload is smaller than OrderWireFormat; callers must drop such
    // frames rather than feed a partially-populated Order into the book.
    //
    // Deliberately NOT touched here:
    //   - out.timestamp_ns: stamped as T1 by the RX thread immediately
    //     around this call (pipeline.cpp), not carried on the wire, so
    //     latency measurement reflects this host's clock, not the
    //     sender's.
    //   - out.event_tag: not carried on the wire either (this protocol has
    //     no room for it) -- relies on the same ObjectPool::acquire()
    //     value-initialization guarantee as prev_/next_ below to come out
    //     as OrderEventTag::NEW_OR_CANCEL (== 0), which is the only event
    //     type this RX path ever produces.
    //   - out.prev_ / out.next_: intrusive FIFO-list pointers owned by
    //     order_book.hpp, set only when the order is actually inserted
    //     into a price level's FIFO. A freshly parsed Order that hasn't
    //     been inserted anywhere yet must not carry stale/garbage list
    //     pointers -- callers are expected to parse into an Order that
    //     came from ObjectPool::acquire() (whose placement-new T()
    //     value-initializes prev_/next_ to nullptr per
    //     test_phase1_types.cpp's test_order_default_state), not to
    //     reuse an Order that was already resting in the book.
    //   - out.client_tag: this order-entry protocol has no room for a
    //     free-text client tag; left as whatever the destination Order
    //     already had (typically zero-initialized).
    namespace detail
    {
        // ING-06: Ethernet(14)+IPv4(20)+UDP(8) puts the IPv4 header at a
        // 2-mod-4 offset and OrderWireFormat at offset 42 (2-mod-8), so reading
        // multi-byte fields through a reinterpret_cast<const iphdr*>/<...*> is a
        // misaligned load -- UB that UBSan flags on every frame. Read every
        // multi-byte field with a byte-wise copy instead. This is NOT a packet
        // copy: nothing is staged through an intermediate buffer, and the
        // payload is still read in place field by field -- the zero-copy
        // property that matters (no per-packet allocation / no full-frame
        // memcpy on the hot path) is preserved.
        template <typename T>
        [[nodiscard]] inline T rd(const uint8_t *p) noexcept
        {
            T v;
            std::memcpy(&v, p, sizeof(T));
            return v;
        }
    } // namespace detail

    [[nodiscard]] inline bool parse_order_zero_copy(const uint8_t *frame,
                                                     uint32_t frame_len,
                                                     hydra::Order &out) noexcept
    {
        std::size_t offset = 0;

        if (frame == nullptr || frame_len < offset + sizeof(ethhdr))
        {
            return false;
        }
        // eth->h_proto sits at offset 12; read it unaligned-safe.
        uint16_t ethertype = detail::rd<uint16_t>(frame + 12);
        offset += sizeof(ethhdr);

        if (ethertype == htons(ETH_P_8021Q) || ethertype == htons(ETH_P_8021AD))
        {
            // Single VLAN tag only -- matches this parser's bounded, non-looping
            // design. A double-tagged (QinQ) frame is rejected here (the eBPF
            // classifier accepts up to 2 tags, so a QinQ order frame would be
            // redirected and then counted as a parse_error rather than misread).
            if (frame_len < offset + sizeof(VlanTag))
            {
                return false;
            }
            ethertype = detail::rd<uint16_t>(frame + offset + offsetof(VlanTag, encapsulated_proto));
            offset += sizeof(VlanTag);
        }
        if (ethertype != htons(ETH_P_IP))
        {
            return false;
        }

        if (frame_len < offset + sizeof(iphdr))
        {
            return false;
        }
        // IPv4 header: version/ihl in byte 0, tot_len at 2, frag_off at 6,
        // protocol at 9 (all read unaligned-safe).
        const uint8_t version_ihl = frame[offset];
        // ING-04: reject anything that isn't IPv4 at the version-field level.
        if ((version_ihl >> 4) != 4)
        {
            return false;
        }
        const std::size_t ip_hdr_len = static_cast<std::size_t>(version_ihl & 0x0F) * 4;
        if (ip_hdr_len < sizeof(iphdr) || frame_len < offset + ip_hdr_len)
        {
            return false;
        }
        if (frame[offset + offsetof(iphdr, protocol)] != IPPROTO_UDP)
        {
            return false;
        }
        // ING-03: reject fragments (MF flag or non-zero fragment offset): a
        // non-initial fragment has protocol==UDP but no UDP header at this
        // offset. frag_off is network byte order.
        const uint16_t frag_off = ntohs(detail::rd<uint16_t>(frame + offset + offsetof(iphdr, frag_off)));
        if (frag_off & (0x2000 /*MF*/ | 0x1FFF /*offset*/))
        {
            return false;
        }
        // ING-02: the IP total length must at least span the IP+UDP+payload we
        // are about to read, and must not claim more than the frame holds.
        const uint16_t ip_tot_len = ntohs(detail::rd<uint16_t>(frame + offset + offsetof(iphdr, tot_len)));
        if (ip_tot_len < ip_hdr_len + sizeof(udphdr) + sizeof(OrderWireFormat) ||
            offset + ip_tot_len > frame_len)
        {
            return false;
        }
        const std::size_t udp_off = offset + ip_hdr_len;

        if (frame_len < udp_off + sizeof(udphdr))
        {
            return false;
        }
        // ING-03: only the order-entry UDP destination port is accepted, so the
        // parser agrees with the eBPF classifier's port filter even when fed
        // from another source (e.g. a raw frame in a test or a mis-steered map).
        const uint16_t udp_dest = ntohs(detail::rd<uint16_t>(frame + udp_off + offsetof(udphdr, dest)));
        if (udp_dest != hydra::config::AFXDP_ORDER_ENTRY_UDP_PORT)
        {
            return false;
        }
        // ING-02: the UDP length must cover its own header plus the full order
        // payload (it must not under-claim the bytes we are about to read).
        const uint16_t udp_len = ntohs(detail::rd<uint16_t>(frame + udp_off + offsetof(udphdr, len)));
        if (udp_len < sizeof(udphdr) + sizeof(OrderWireFormat))
        {
            return false;
        }
        const std::size_t wire_off = udp_off + sizeof(udphdr);

        if (frame_len < wire_off + sizeof(OrderWireFormat))
        {
            return false;
        }

        const uint64_t order_id = be64toh(detail::rd<uint64_t>(frame + wire_off + offsetof(OrderWireFormat, order_id)));
        const int64_t price = detail::ntoh64(detail::rd<uint64_t>(frame + wire_off + offsetof(OrderWireFormat, price)));
        const uint32_t qty = ntohl(detail::rd<uint32_t>(frame + wire_off + offsetof(OrderWireFormat, qty)));
        const uint64_t client_id = be64toh(detail::rd<uint64_t>(frame + wire_off + offsetof(OrderWireFormat, client_id)));
        const uint8_t side = frame[wire_off + offsetof(OrderWireFormat, side)];
        const uint8_t tif = frame[wire_off + offsetof(OrderWireFormat, tif)];

        // ING-01 / ING-05: validate the fields the matcher will trust. side and
        // tif must be in range (an out-of-range enum byte is UB downstream); a
        // zero quantity on the wire would become an unauthenticated cancel of
        // order_id in matching_thread_fn (qty==0 == cancel), so reject it -- the
        // wire protocol has no cancel message.
        if (side > static_cast<uint8_t>(hydra::Side::SELL) ||
            tif > static_cast<uint8_t>(hydra::TimeInForce::FOK) ||
            qty == 0)
        {
            return false;
        }

        out.order_id = order_id;
        out.price = price;
        out.qty = qty;
        out.side = static_cast<hydra::Side>(side);
        out.tif = static_cast<hydra::TimeInForce>(tif);
        out.client_id = client_id;

        return true;
    }

    // Convenience wrapper: parses the frame described by `desc` directly
    // out of `socket`'s UMEM, then unconditionally returns the frame to
    // the free-frame pool (and, via the next recv_batch(), the FILL ring)
    // so the kernel can reuse the slot -- regardless of whether parsing
    // succeeded, per the AF_XDP contract that every frame handed out by
    // recv_batch() must eventually be released or the UMEM starves.
    [[nodiscard]] inline bool parse_and_release(XdpSocket &socket,
                                                 const FrameDescriptor &desc,
                                                 hydra::Order &out) noexcept
    {
        const bool ok =
            parse_order_zero_copy(socket.frame_data(desc.addr), desc.len, out);
        socket.release_frame(desc.addr);
        return ok;
    }

} // namespace hydra::xdp

#endif // ENABLE_AFXDP
