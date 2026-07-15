//===----------------------------------------------------------------------===
// tests/test_phase10_xdp.cpp
//
// Phase 10 exit condition (see HYDRA-LOB-Roadmap.md):
//   "non-AFXDP build compiles/tests pass with ENABLE_AFXDP undefined; with
//   it defined and libbpf present, the AFXDP variant compiles."
//
// This is a COMPILE-GATE smoke test, not a runtime AF_XDP integration test:
// opening a real XdpSocket needs root, a bound network interface, and (for
// a meaningful zero-copy result) a NIC/driver combination this suite can't
// assume it's running on -- exactly why the roadmap treats a real AF_XDP
// run as "best-effort, manual, never a CI gate" (see xdp_socket.hpp's own
// WHY comments). What CAN be tested here, with no root and no real socket,
// is include/hydra/xdp/zero_copy_parser.hpp's parse_order_zero_copy(): it's
// a pure function over a byte buffer, so a synthetic in-memory
// Ethernet+IPv4+UDP+OrderWireFormat frame exercises the exact same code
// path recv_batch()+frame_data() would hand it in production, without any
// of the setup a real socket needs.
//
// With ENABLE_AFXDP undefined, xdp_socket.hpp/zero_copy_parser.hpp compile
// to nothing (clean #ifdef guards) -- this file mirrors that with a single
// trivial pass, proving the rest of the test suite (and blitz_lob itself)
// builds and runs untouched in that configuration, per the exit condition
// above.
//===----------------------------------------------------------------------===

#include "test_harness.hpp"

#ifdef ENABLE_AFXDP

#include "hydra/config.hpp"
#include "hydra/types.hpp"
#include "hydra/xdp/xdp_socket.hpp"
#include "hydra/xdp/zero_copy_parser.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <endian.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <vector>

namespace hydra::test
{
    namespace
    {

        // Fields chosen to be individually distinguishable (no repeated
        // digits) so a field-swap bug in parse_order_zero_copy() would show
        // up as a mismatched value here rather than accidentally matching.
        struct SyntheticOrder
        {
            uint64_t order_id = 0x1122334455667788ULL;
            int64_t price = 100'250;
            uint32_t qty = 777;
            uint64_t client_id = 0xAABBCCDDEEFF0011ULL;
            uint8_t side = 1; // hydra::Side::SELL
            uint8_t tif = 1;  // hydra::TimeInForce::IOC
        };

        // Builds a well-formed Ethernet + IPv4 + UDP + OrderWireFormat
        // frame into `buf` and returns its total length. Mirrors exactly
        // what net/xdp_prog.bpf.c redirects and XdpSocket::frame_data()
        // would point at -- this is the same byte layout, just assembled
        // in userspace instead of arriving over a veth pair.
        [[nodiscard]] std::size_t build_synthetic_frame(std::vector<uint8_t> &buf,
                                                          const SyntheticOrder &order)
        {
            buf.assign(sizeof(ethhdr) + sizeof(iphdr) + sizeof(udphdr) +
                           sizeof(hydra::xdp::OrderWireFormat),
                       0);
            std::size_t offset = 0;

            auto *eth = reinterpret_cast<ethhdr *>(buf.data() + offset);
            eth->h_proto = htons(ETH_P_IP);
            offset += sizeof(ethhdr);

            auto *ip = reinterpret_cast<iphdr *>(buf.data() + offset);
            ip->ihl = 5; // no options -> 20-byte header
            ip->version = 4;
            ip->protocol = IPPROTO_UDP;
            offset += sizeof(iphdr);

            auto *udp = reinterpret_cast<udphdr *>(buf.data() + offset);
            udp->len = htons(static_cast<uint16_t>(
                sizeof(udphdr) + sizeof(hydra::xdp::OrderWireFormat)));
            offset += sizeof(udphdr);

            auto *wire =
                reinterpret_cast<hydra::xdp::OrderWireFormat *>(buf.data() + offset);
            wire->order_id = htobe64(order.order_id);
            wire->price = static_cast<int64_t>(htobe64(static_cast<uint64_t>(order.price)));
            wire->qty = htonl(order.qty);
            wire->client_id = htobe64(order.client_id);
            wire->side = order.side;
            wire->tif = order.tif;

            return buf.size();
        }

        // Inserts `num_tags` 4-byte 802.1Q tags between the Ethernet
        // header and the IPv4 header of an otherwise well-formed frame
        // (built via build_synthetic_frame() above), so the resulting
        // frame's outer eth->h_proto reads ETH_P_8021Q and the true
        // ETH_P_IP ethertype is num_tags tags deeper -- exactly the shape
        // parse_order_zero_copy()'s VLAN handling (zero_copy_parser.hpp)
        // needs to unwrap correctly.
        [[nodiscard]] std::size_t build_vlan_tagged_frame(std::vector<uint8_t> &buf,
                                                            const SyntheticOrder &order,
                                                            int num_tags)
        {
            std::vector<uint8_t> untagged;
            const std::size_t untagged_len = build_synthetic_frame(untagged, order);

            buf.assign(untagged_len + static_cast<std::size_t>(num_tags) * sizeof(hydra::xdp::VlanTag), 0);

            std::memcpy(buf.data(), untagged.data(), sizeof(ethhdr));
            auto *eth = reinterpret_cast<ethhdr *>(buf.data());
            eth->h_proto = htons(ETH_P_8021Q);

            std::size_t offset = sizeof(ethhdr);
            for (int i = 0; i < num_tags; ++i)
            {
                auto *vlan = reinterpret_cast<hydra::xdp::VlanTag *>(buf.data() + offset);
                vlan->tci = htons(42); // arbitrary VLAN ID, not inspected by the parser
                vlan->encapsulated_proto =
                    (i + 1 < num_tags) ? htons(ETH_P_8021Q) : htons(ETH_P_IP);
                offset += sizeof(hydra::xdp::VlanTag);
            }

            std::memcpy(buf.data() + offset, untagged.data() + sizeof(ethhdr),
                        untagged_len - sizeof(ethhdr));

            return buf.size();
        }

        void test_order_wire_format_size()
        {
            HYDRA_CHECK_EQ(sizeof(hydra::xdp::OrderWireFormat), std::size_t{30});
        }

        void test_parse_valid_frame_extracts_all_fields()
        {
            SyntheticOrder synth{};
            std::vector<uint8_t> buf;
            const std::size_t len = build_synthetic_frame(buf, synth);

            hydra::Order out{};
            out.timestamp_ns = 0xDEADBEEFULL; // sentinel: parser must not touch this

            const bool ok = hydra::xdp::parse_order_zero_copy(
                buf.data(), static_cast<uint32_t>(len), out);

            HYDRA_CHECK(ok);
            HYDRA_CHECK_EQ(out.order_id, synth.order_id);
            HYDRA_CHECK_EQ(out.price, synth.price);
            HYDRA_CHECK_EQ(out.qty, synth.qty);
            HYDRA_CHECK_EQ(out.client_id, synth.client_id);
            HYDRA_CHECK(out.side == hydra::Side::SELL);
            HYDRA_CHECK(out.tif == hydra::TimeInForce::IOC);

            // Deliberately untouched fields, per zero_copy_parser.hpp's WHY
            // comment: timestamp_ns is stamped by the RX thread, not
            // carried on the wire; prev_/next_ belong to order_book.hpp.
            HYDRA_CHECK_EQ(out.timestamp_ns, uint64_t{0xDEADBEEF});
            HYDRA_CHECK(out.prev_ == nullptr);
            HYDRA_CHECK(out.next_ == nullptr);
        }

        void test_parse_rejects_non_ip_ethertype()
        {
            SyntheticOrder synth{};
            std::vector<uint8_t> buf;
            const std::size_t len = build_synthetic_frame(buf, synth);

            auto *eth = reinterpret_cast<ethhdr *>(buf.data());
            eth->h_proto = htons(ETH_P_ARP);

            hydra::Order out{};
            const bool ok = hydra::xdp::parse_order_zero_copy(
                buf.data(), static_cast<uint32_t>(len), out);
            HYDRA_CHECK(!ok);
        }

        void test_parse_rejects_non_udp_protocol()
        {
            SyntheticOrder synth{};
            std::vector<uint8_t> buf;
            const std::size_t len = build_synthetic_frame(buf, synth);

            auto *ip = reinterpret_cast<iphdr *>(buf.data() + sizeof(ethhdr));
            ip->protocol = IPPROTO_TCP;

            hydra::Order out{};
            const bool ok = hydra::xdp::parse_order_zero_copy(
                buf.data(), static_cast<uint32_t>(len), out);
            HYDRA_CHECK(!ok);
        }

        void test_parse_rejects_truncated_frame()
        {
            SyntheticOrder synth{};
            std::vector<uint8_t> buf;
            const std::size_t len = build_synthetic_frame(buf, synth);

            hydra::Order out{};
            // One byte short of a complete OrderWireFormat payload.
            const bool ok = hydra::xdp::parse_order_zero_copy(
                buf.data(), static_cast<uint32_t>(len - 1), out);
            HYDRA_CHECK(!ok);
        }

        void test_parse_rejects_empty_frame()
        {
            hydra::Order out{};
            const bool ok = hydra::xdp::parse_order_zero_copy(nullptr, 0, out);
            HYDRA_CHECK(!ok);
        }

        // Regression test: net/xdp_prog.bpf.c's classifier (via
        // parsing_helpers.h's parse_ethhdr_vlan()) already skips a single
        // VLAN tag when deciding whether to redirect a frame, so a
        // VLAN-tagged order-entry packet reaches this parser -- it must
        // not be rejected just because eth->h_proto reads ETH_P_8021Q
        // instead of ETH_P_IP.
        void test_parse_accepts_single_vlan_tag()
        {
            SyntheticOrder synth{};
            std::vector<uint8_t> buf;
            const std::size_t len = build_vlan_tagged_frame(buf, synth, 1);

            hydra::Order out{};
            const bool ok = hydra::xdp::parse_order_zero_copy(
                buf.data(), static_cast<uint32_t>(len), out);

            HYDRA_CHECK(ok);
            HYDRA_CHECK_EQ(out.order_id, synth.order_id);
            HYDRA_CHECK_EQ(out.price, synth.price);
            HYDRA_CHECK_EQ(out.qty, synth.qty);
            HYDRA_CHECK_EQ(out.client_id, synth.client_id);
            HYDRA_CHECK(out.side == hydra::Side::SELL);
            HYDRA_CHECK(out.tif == hydra::TimeInForce::IOC);
        }

        // Double-tagged (QinQ) frames are deliberately rejected -- this
        // parser only unwraps one VLAN tag, matching zero_copy_parser.hpp's
        // documented bounded-parsing design (see its WHY comment). Proves
        // that design choice actually rejects rather than misreads.
        void test_parse_rejects_double_vlan_tag()
        {
            SyntheticOrder synth{};
            std::vector<uint8_t> buf;
            const std::size_t len = build_vlan_tagged_frame(buf, synth, 2);

            hydra::Order out{};
            const bool ok = hydra::xdp::parse_order_zero_copy(
                buf.data(), static_cast<uint32_t>(len), out);
            HYDRA_CHECK(!ok);
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_order_wire_format_size);
    RUN_TEST(test_parse_valid_frame_extracts_all_fields);
    RUN_TEST(test_parse_rejects_non_ip_ethertype);
    RUN_TEST(test_parse_rejects_non_udp_protocol);
    RUN_TEST(test_parse_rejects_truncated_frame);
    RUN_TEST(test_parse_rejects_empty_frame);
    RUN_TEST(test_parse_accepts_single_vlan_tag);
    RUN_TEST(test_parse_rejects_double_vlan_tag);

    return report_and_exit_code();
}

#else // !ENABLE_AFXDP

namespace hydra::test
{
    namespace
    {

        void test_afxdp_disabled_trivial_pass()
        {
            // Nothing to test: xdp_socket.hpp/zero_copy_parser.hpp compile
            // to nothing in this configuration. This test's only job is to
            // exist and pass, proving the binary still builds and runs
            // cleanly with ENABLE_AFXDP off, per Phase 10's exit condition.
            HYDRA_CHECK(true);
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_afxdp_disabled_trivial_pass);

    return report_and_exit_code();
}

#endif // ENABLE_AFXDP
