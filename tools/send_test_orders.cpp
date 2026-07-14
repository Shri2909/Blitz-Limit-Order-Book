// tools/send_test_orders.cpp
//
// HYDRA-LOB Phase 10 -- manual AF_XDP test traffic generator.
//
// Fires OrderWireFormat-shaped UDP datagrams (hydra/xdp/zero_copy_parser.hpp)
// at a target so a human can drive real packets through the real
// afxdp_rx_thread_fn path (src/pipeline.cpp) end-to-end, typically over the
// veth pair scripts/setup_veth.sh creates. Reuses OrderWireFormat directly
// rather than re-declaring the wire layout here, so sender and parser can
// never silently drift apart.
//
// Deliberately a normal SOCK_DGRAM UDP socket, not an AF_XDP socket itself:
// the kernel's own UDP/IP stack builds correct Ethernet+IPv4+UDP headers,
// which is exactly the frame shape net/xdp_prog.bpf.c and
// parse_order_zero_copy() expect -- no need to hand-roll those headers here.
//
// Only built when ENABLE_AFXDP is ON (needs zero_copy_parser.hpp's
// OrderWireFormat, itself gated the same way -- see CMakeLists.txt).

#ifndef ENABLE_AFXDP
#error "tools/send_test_orders.cpp must only be built with -DENABLE_AFXDP=ON"
#endif

#include "hydra/types.hpp"
#include "hydra/xdp/zero_copy_parser.hpp"

#include <arpa/inet.h>
#include <endian.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{

    void print_usage(const char *prog)
    {
        std::fprintf(stderr,
                     "Usage: %s --target IP:PORT [--count N] [--rate-hz F]\n"
                     "          [--seed N] [--client-id N]\n"
                     "\n"
                     "  --target IP:PORT  required: destination for the UDP order-entry\n"
                     "                    traffic, e.g. 10.200.0.1:40000 (veth-hydra0's\n"
                     "                    address + hydra::config::AFXDP_ORDER_ENTRY_UDP_PORT)\n"
                     "  --count N         number of orders to send (default: 1000)\n"
                     "  --rate-hz F       pace sends at this rate; 0 = send as fast as\n"
                     "                    possible (default: 0)\n"
                     "  --seed N          RNG seed for price/qty/side (default: 42)\n"
                     "  --client-id N     client_id stamped on every order (default: 1)\n"
                     "\n"
                     "Example:\n"
                     "  %s --target 10.200.0.1:40000 --count 2000 --rate-hz 500\n",
                     prog, prog);
    }

    bool match_flag(int argc, char **argv, int &i, const char *flag, std::string &out_value)
    {
        if (std::strcmp(argv[i], flag) != 0)
        {
            return false;
        }
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "error: %s requires a value\n", flag);
            std::exit(1);
        }
        out_value = argv[++i];
        return true;
    }

    unsigned long long parse_u64(const std::string &s, const char *flag)
    {
        try
        {
            std::size_t consumed = 0;
            const unsigned long long v = std::stoull(s, &consumed);
            if (consumed != s.size())
            {
                throw std::invalid_argument("trailing characters");
            }
            return v;
        }
        catch (const std::exception &)
        {
            std::fprintf(stderr, "error: %s expects a non-negative integer, got '%s'\n",
                         flag, s.c_str());
            std::exit(1);
        }
    }

    double parse_double(const std::string &s, const char *flag)
    {
        try
        {
            std::size_t consumed = 0;
            const double v = std::stod(s, &consumed);
            if (consumed != s.size())
            {
                throw std::invalid_argument("trailing characters");
            }
            return v;
        }
        catch (const std::exception &)
        {
            std::fprintf(stderr, "error: %s expects a number, got '%s'\n", flag, s.c_str());
            std::exit(1);
        }
    }

    struct Target
    {
        std::string ip;
        uint16_t port;
    };

    Target parse_target(const std::string &s)
    {
        const auto colon = s.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == s.size())
        {
            std::fprintf(stderr, "error: --target expects IP:PORT, got '%s'\n", s.c_str());
            std::exit(1);
        }
        Target t;
        t.ip = s.substr(0, colon);
        t.port = static_cast<uint16_t>(parse_u64(s.substr(colon + 1), "--target port"));
        return t;
    }

} // namespace

int main(int argc, char **argv)
{
    std::string target_str;
    std::size_t count = 1000;
    double rate_hz = 0.0;
    uint64_t seed = 42;
    uint64_t client_id = 1;

    for (int i = 1; i < argc; ++i)
    {
        std::string val;
        if (match_flag(argc, argv, i, "--target", val))
        {
            target_str = val;
            continue;
        }
        if (match_flag(argc, argv, i, "--count", val))
        {
            count = static_cast<std::size_t>(parse_u64(val, "--count"));
            continue;
        }
        if (match_flag(argc, argv, i, "--rate-hz", val))
        {
            rate_hz = parse_double(val, "--rate-hz");
            continue;
        }
        if (match_flag(argc, argv, i, "--seed", val))
        {
            seed = parse_u64(val, "--seed");
            continue;
        }
        if (match_flag(argc, argv, i, "--client-id", val))
        {
            client_id = parse_u64(val, "--client-id");
            continue;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        std::fprintf(stderr, "error: unknown flag '%s'\n", argv[i]);
        print_usage(argv[0]);
        return 1;
    }

    if (target_str.empty())
    {
        std::fprintf(stderr, "error: --target IP:PORT is required\n");
        print_usage(argv[0]);
        return 1;
    }
    const Target target = parse_target(target_str);

    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        std::fprintf(stderr, "error: socket() failed: %s\n", std::strerror(errno));
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(target.port);
    if (inet_pton(AF_INET, target.ip.c_str(), &addr.sin_addr) != 1)
    {
        std::fprintf(stderr, "error: invalid target IP '%s'\n", target.ip.c_str());
        close(sock);
        return 1;
    }
    if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        std::fprintf(stderr, "error: connect() to %s:%u failed: %s\n", target.ip.c_str(),
                     target.port, std::strerror(errno));
        close(sock);
        return 1;
    }

    std::printf("send_test_orders: target=%s:%u count=%zu rate_hz=%.1f seed=%llu client_id=%llu\n",
                target.ip.c_str(), target.port, count, rate_hz,
                static_cast<unsigned long long>(seed), static_cast<unsigned long long>(client_id));

    // Same synthetic field distributions as rx_thread_fn's in-process
    // generator (src/pipeline.cpp) -- side alternates by order_id parity,
    // tif is always GTC -- so packets sent here exercise the same order
    // shape the synthetic path would have produced.
    std::mt19937_64 rng{seed};
    std::uniform_int_distribution<int64_t> price_dist(99'000, 101'000);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 100);

    const auto interval = (rate_hz > 0.0)
                              ? std::chrono::duration<double>(1.0 / rate_hz)
                              : std::chrono::duration<double>(0.0);

    std::size_t sent = 0;
    for (uint64_t order_id = 1; order_id <= count; ++order_id)
    {
        hydra::xdp::OrderWireFormat wire{};
        wire.order_id = htobe64(order_id);
        wire.price = static_cast<int64_t>(htobe64(static_cast<uint64_t>(price_dist(rng))));
        wire.qty = htonl(qty_dist(rng));
        wire.client_id = htobe64(client_id);
        wire.side = (order_id % 2 == 0) ? static_cast<uint8_t>(hydra::Side::BUY)
                                        : static_cast<uint8_t>(hydra::Side::SELL);
        wire.tif = static_cast<uint8_t>(hydra::TimeInForce::GTC);

        const ssize_t n = send(sock, &wire, sizeof(wire), 0);
        if (n != static_cast<ssize_t>(sizeof(wire)))
        {
            std::fprintf(stderr, "error: send() failed at order %llu: %s\n",
                         static_cast<unsigned long long>(order_id), std::strerror(errno));
            close(sock);
            return 1;
        }
        ++sent;

        if (rate_hz > 0.0)
        {
            std::this_thread::sleep_for(interval);
        }
    }

    close(sock);
    std::printf("send_test_orders: sent %zu order(s) to %s:%u\n", sent, target.ip.c_str(),
                target.port);
    return 0;
}
