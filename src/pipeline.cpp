#include "hydra/pipeline.hpp"

#include "hydra/affinity.hpp"
#include "hydra/clock.hpp"

#include <algorithm>
#include <cstdio>
#include <random>

#ifdef ENABLE_AFXDP
#include <memory>

#include "hydra/xdp/zero_copy_parser.hpp"
#endif

namespace hydra
{

    namespace
    {

        constexpr int kBoundedSpinAttempts = 1000;

    }

    void rx_thread_fn(std::stop_token stop, PipelineContext &ctx)
    {
        try
        {
            pin_to_core(RX_CORE_ID);
            verify_affinity(RX_CORE_ID);
        }
        catch (const std::runtime_error &e)
        {
            std::fprintf(stderr,
                         "rx_thread_fn: affinity setup failed, exiting without "
                         "entering its loop: %s\n",
                         e.what());
            return;
        }

        const double ns_per_cycle = ctx.ns_per_cycle;

        std::mt19937_64 rng{std::random_device{}()};
        std::uniform_int_distribution<int64_t> price_dist(99'000, 101'000);
        std::uniform_int_distribution<uint32_t> qty_dist(1, 100);
        uint64_t next_order_id = 1;
        uint64_t dropped_orders = 0;

        while (!stop.stop_requested())
        {
            Order order{};
            order.order_id = next_order_id++;
            order.price = price_dist(rng);
            order.qty = qty_dist(rng);
            order.side = (order.order_id % 2 == 0) ? Side::BUY : Side::SELL;
            order.tif = TimeInForce::GTC;
            order.timestamp_ns = static_cast<uint64_t>(
                static_cast<double>(rdtsc_now()) * ns_per_cycle);

            bool pushed = false;
            for (int attempt = 0;
                 attempt < kBoundedSpinAttempts && !stop.stop_requested();
                 ++attempt)
            {
                if (ctx.queue.push(order))
                {
                    pushed = true;
                    break;
                }
            }
            if (!pushed) [[unlikely]]
            {
                ++dropped_orders;
            }
        }

        if (dropped_orders > 0) [[unlikely]]
        {
            std::fprintf(stderr,
                         "rx_thread_fn: dropped %llu order(s) to sustained "
                         "queue backpressure before shutdown\n",
                         static_cast<unsigned long long>(dropped_orders));
        }
    }

    void matching_thread_fn(std::stop_token stop, PipelineContext &ctx)
    {
        try
        {
            pin_to_core(MATCHING_CORE_ID);
            verify_affinity(MATCHING_CORE_ID);
        }
        catch (const std::runtime_error &e)
        {
            std::fprintf(stderr,
                         "matching_thread_fn: affinity setup failed, exiting "
                         "without entering its loop: %s\n",
                         e.what());
            return;
        }

        const double ns_per_cycle = ctx.ns_per_cycle;
        const auto now_ns = [ns_per_cycle]() -> uint64_t
        {
            return static_cast<uint64_t>(static_cast<double>(rdtsc_now()) * ns_per_cycle);
        };

        Order order{};
        while (!stop.stop_requested())
        {
            bool popped = false;
            uint64_t t_pop_start_ns = 0;
            uint64_t t_pop_end_ns = 0;
            for (int attempt = 0;
                 attempt < kBoundedSpinAttempts && !stop.stop_requested();
                 ++attempt)
            {
                t_pop_start_ns = now_ns();
                if (ctx.queue.pop(order))
                {
                    t_pop_end_ns = now_ns();
                    popped = true;
                    break;
                }
            }
            if (!popped)
            {
                continue;
            }

            const uint64_t t1_ns = order.timestamp_ns;
            const uint64_t queue_residence_ns = t_pop_start_ns - t1_ns;
            const uint64_t queue_pop_ns = t_pop_end_ns - t_pop_start_ns;

            if (order.event_tag == OrderEventTag::NEW_OR_CANCEL && order.qty == 0) [[unlikely]]
            {
                // Cancels used to record an all-zero sample here and were
                // excluded from every reported figure; every recorded
                // sample is now here, distinguishable via is_cancel, not
                // just the order/replace ones.
                const uint64_t tc_start_ns = now_ns();

                // Discarded intentionally: a cancel referencing an order
                // that's already filled/cancelled is a normal race in a
                // live book (or a replayed dataset), not an error condition
                // this hot path needs to react to.
                [[maybe_unused]] const bool cancelled = ctx.book.cancel_order(order.order_id);

                const uint64_t tc_end_ns = now_ns();

                RawSampleSink *sink = ctx.raw_samples.load(std::memory_order_acquire);
                if (sink != nullptr)
                {
                    LatencySample s{};
                    s.queue_residence_ns = queue_residence_ns;
                    s.queue_pop_ns = queue_pop_ns;
                    s.match_time_ns = tc_end_ns - tc_start_ns;
                    s.end_to_end_ns = tc_end_ns - t1_ns;
                    s.is_cancel = true;
                    sink->record(s);
                }
                continue;
            }

            // Cumulative on_fill() time, bracketed separately from the
            // surrounding match_time_ns so book-mutation cost and
            // fill-delivery cost are cleanly partitioned rather than fused
            // into one number -- see LatencySample::fill_publish_ns.
            uint64_t fill_publish_ns = 0;
            const auto timed_on_fill = [&](const FillEvent &fill)
            {
                const uint64_t pub_start_ns = now_ns();
                (void)fill;
                const uint64_t pub_end_ns = now_ns();
                fill_publish_ns += (pub_end_ns - pub_start_ns);
            };

            const bool is_replace = (order.event_tag == OrderEventTag::REPLACE);

            const uint64_t t_match_start_ns = now_ns();
            const MatchStats stats = is_replace
                                         ? ctx.matcher.replace(order.order_id, order.price,
                                                               order.qty, timed_on_fill)
                                         : ctx.matcher.match(order, timed_on_fill);
            const uint64_t t_match_end_ns = now_ns();

            const uint64_t match_time_ns =
                (t_match_end_ns - t_match_start_ns) -
                std::min(fill_publish_ns, t_match_end_ns - t_match_start_ns);
            const uint64_t end_to_end_ns = t_match_end_ns - t1_ns;

            RawSampleSink *sink = ctx.raw_samples.load(std::memory_order_acquire);
            if (sink != nullptr)
            {
                LatencySample s{};
                s.queue_residence_ns = queue_residence_ns;
                s.queue_pop_ns = queue_pop_ns;
                s.match_time_ns = match_time_ns;
                s.fill_publish_ns = fill_publish_ns;
                s.end_to_end_ns = end_to_end_ns;
                s.fills_generated = stats.fills_generated;
                s.levels_consumed = stats.levels_consumed;
                s.resting_orders_examined = stats.resting_orders_examined;
                s.eligible_orders_examined = stats.eligible_orders_examined;
                s.self_trade_skips = stats.self_trade_skips;
                s.remaining_qty = stats.remaining_qty;
                s.is_replace = is_replace;
                sink->record(s);
            }
        }
    }

#ifdef ENABLE_AFXDP

    namespace
    {

        [[nodiscard]] const char *attach_mode_name(xdp_attach_mode mode) noexcept
        {
            switch (mode)
            {
            case XDP_MODE_NATIVE:
                return "native";
            case XDP_MODE_SKB:
                return "skb";
            case XDP_MODE_HW:
                return "hw";
            default:
                return "unspec";
            }
        }

    } // namespace

    // Real-traffic RX path: drains an AF_XDP socket instead of synthesizing
    // orders (contrast with rx_thread_fn above). Mirrors rx_thread_fn's
    // affinity-setup, timestamping, and bounded-spin-then-drop push
    // conventions exactly -- the only new behavior is the periodic progress
    // line, added because a manual veth test has no other way to observe
    // that packets are actually landing (rx_thread_fn's synthetic loop
    // never needed one: its "success" is simply that the pipeline runs at
    // all).
    void afxdp_rx_thread_fn(std::stop_token stop, PipelineContext &ctx, const AfxdpConfig &cfg)
    {
        try
        {
            pin_to_core(RX_CORE_ID);
            verify_affinity(RX_CORE_ID);
        }
        catch (const std::runtime_error &e)
        {
            std::fprintf(stderr,
                         "afxdp_rx_thread_fn: affinity setup failed, exiting without "
                         "entering its loop: %s\n",
                         e.what());
            return;
        }

        xdp_program *prog = nullptr;
        try
        {
            prog = xdp::load_and_attach_xdp_program(cfg.ifname, cfg.bpf_prog_path, "xdp",
                                                     cfg.attach_mode);
        }
        catch (const std::runtime_error &e)
        {
            std::fprintf(stderr, "afxdp_rx_thread_fn: failed to load/attach XDP program: %s\n",
                         e.what());
            return;
        }

        std::unique_ptr<xdp::XdpSocket> socket;
        try
        {
            const int xsks_map_fd = xdp::find_xsks_map_fd(prog);
            socket = std::make_unique<xdp::XdpSocket>(cfg.ifname, cfg.queue_id,
                                                       config::AFXDP_UMEM_SIZE, xsks_map_fd);
        }
        catch (const std::runtime_error &e)
        {
            std::fprintf(stderr, "afxdp_rx_thread_fn: failed to open AF_XDP socket: %s\n",
                         e.what());
            xdp::detach_xdp_program(prog, cfg.ifname, cfg.attach_mode);
            return;
        }

        std::fprintf(stdout,
                     "afxdp_rx_thread_fn: bound to %s queue %u (prog=%s, mode=%s)\n",
                     cfg.ifname.c_str(), cfg.queue_id, cfg.bpf_prog_path.c_str(),
                     attach_mode_name(cfg.attach_mode));

        const double ns_per_cycle = ctx.ns_per_cycle;

        uint64_t received = 0;
        uint64_t parsed_ok = 0;
        uint64_t parse_errors = 0;
        uint64_t dropped = 0;

        // Progress reporting only, never on the frame-processing path --
        // rdtsc_now() is cheap enough to call unconditionally each outer
        // loop iteration, but the fprintf itself is gated to ~once per 5s.
        constexpr uint64_t kProgressIntervalNs = 5'000'000'000ULL;
        uint64_t last_progress_cycles = rdtsc_now();

        xdp::FrameDescriptor batch[config::AFXDP_RX_BATCH_SIZE];

        while (!stop.stop_requested())
        {
            const std::size_t n = socket->recv_batch(batch, config::AFXDP_RX_BATCH_SIZE);

            for (std::size_t i = 0; i < n; ++i)
            {
                ++received;
                Order order{};
                if (!xdp::parse_and_release(*socket, batch[i], order))
                {
                    ++parse_errors;
                    continue;
                }

                order.timestamp_ns = static_cast<uint64_t>(
                    static_cast<double>(rdtsc_now()) * ns_per_cycle);

                bool pushed = false;
                for (int attempt = 0;
                     attempt < kBoundedSpinAttempts && !stop.stop_requested();
                     ++attempt)
                {
                    if (ctx.queue.push(order))
                    {
                        pushed = true;
                        break;
                    }
                }
                if (pushed)
                {
                    ++parsed_ok;
                }
                else [[unlikely]]
                {
                    ++dropped;
                }
            }

            const uint64_t now_cycles = rdtsc_now();
            const uint64_t elapsed_ns = static_cast<uint64_t>(
                static_cast<double>(now_cycles - last_progress_cycles) * ns_per_cycle);
            if (elapsed_ns >= kProgressIntervalNs)
            {
                std::fprintf(stdout,
                             "afxdp_rx_thread_fn: received=%llu parsed_ok=%llu "
                             "parse_errors=%llu dropped=%llu double_releases=%llu\n",
                             static_cast<unsigned long long>(received),
                             static_cast<unsigned long long>(parsed_ok),
                             static_cast<unsigned long long>(parse_errors),
                             static_cast<unsigned long long>(dropped),
                             static_cast<unsigned long long>(socket->double_release_count()));
                last_progress_cycles = now_cycles;
            }
        }

        // Captured before socket.reset() below -- the counter lives on the
        // XdpSocket instance, which is about to be destroyed.
        const uint64_t double_releases = socket->double_release_count();

        // Socket destructor (UMEM/ring teardown) runs here, before the
        // program is detached -- matches Phase 10's documented shutdown
        // order (xdp_socket.hpp), and mirrors the try/catch paths above
        // which detach on failure only after any partially-constructed
        // socket has already unwound.
        socket.reset();
        xdp::detach_xdp_program(prog, cfg.ifname, cfg.attach_mode);

        std::fprintf(stdout,
                     "afxdp_rx_thread_fn: shutdown complete. final counts: received=%llu "
                     "parsed_ok=%llu parse_errors=%llu dropped=%llu double_releases=%llu\n",
                     static_cast<unsigned long long>(received),
                     static_cast<unsigned long long>(parsed_ok),
                     static_cast<unsigned long long>(parse_errors),
                     static_cast<unsigned long long>(dropped),
                     static_cast<unsigned long long>(double_releases));
    }

#endif // ENABLE_AFXDP

} // namespace hydra
