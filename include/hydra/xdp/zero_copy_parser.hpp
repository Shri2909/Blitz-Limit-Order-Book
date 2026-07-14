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
    [[nodiscard]] inline bool parse_order_zero_copy(const uint8_t *frame,
                                                     uint32_t frame_len,
                                                     hydra::Order &out) noexcept
    {
        std::size_t offset = 0;

        if (frame_len < offset + sizeof(ethhdr))
        {
            return false;
        }
        const auto *eth = reinterpret_cast<const ethhdr *>(frame + offset);
        offset += sizeof(ethhdr);
        if (eth->h_proto != htons(ETH_P_IP))
        {
            return false;
        }

        if (frame_len < offset + sizeof(iphdr))
        {
            return false;
        }
        const auto *ip = reinterpret_cast<const iphdr *>(frame + offset);
        if (ip->protocol != IPPROTO_UDP)
        {
            return false;
        }
        const std::size_t ip_hdr_len = static_cast<std::size_t>(ip->ihl) * 4;
        if (ip_hdr_len < sizeof(iphdr) || frame_len < offset + ip_hdr_len)
        {
            return false;
        }
        offset += ip_hdr_len;

        if (frame_len < offset + sizeof(udphdr))
        {
            return false;
        }
        offset += sizeof(udphdr);

        if (frame_len < offset + sizeof(OrderWireFormat))
        {
            return false;
        }
        const auto *wire = reinterpret_cast<const OrderWireFormat *>(frame + offset);

        out.order_id = be64toh(wire->order_id);
        out.price = detail::ntoh64(static_cast<uint64_t>(wire->price));
        out.qty = ntohl(wire->qty);
        out.side = static_cast<hydra::Side>(wire->side);
        out.tif = static_cast<hydra::TimeInForce>(wire->tif);
        out.client_id = be64toh(wire->client_id);

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
