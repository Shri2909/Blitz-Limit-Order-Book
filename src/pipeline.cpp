#include "hydra/pipeline.hpp"

#include "hydra/affinity.hpp"
#include "hydra/clock.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <stdexcept>

namespace hydra
{

    namespace
    {

        constexpr int kBoundedSpinAttempts = 1000;
        constexpr uint64_t kSyntheticClientCount = 8;

    } // namespace

    void rx_thread_fn(std::stop_token stop, PipelineContext &ctx)
    {
        try
        {
            pin_to_core(RX_CORE_ID);
            verify_affinity(RX_CORE_ID);
        }
        catch (const std::runtime_error &e)
        {
            ctx.rx_thread_startup.store(WorkerStartupState::FAILED, std::memory_order_release);
            std::fprintf(stderr,
                         "rx_thread_fn: affinity setup failed, exiting without "
                         "entering its loop: %s\n",
                         e.what());
            return;
        }

        ctx.rx_thread_startup.store(WorkerStartupState::READY, std::memory_order_release);

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
            // Several clients so self-trade checks are exercised.
            order.client_id = 1 + (order.order_id % kSyntheticClientCount);
            order.timestamp_ns =
                static_cast<uint64_t>(static_cast<double>(rdtsc_now()) * ns_per_cycle);

            bool pushed = false;
            for (int attempt = 0; attempt < kBoundedSpinAttempts && !stop.stop_requested();
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
            ctx.matching_thread_startup.store(WorkerStartupState::FAILED,
                                              std::memory_order_release);
            std::fprintf(stderr,
                         "matching_thread_fn: affinity setup failed, exiting "
                         "without entering its loop: %s\n",
                         e.what());
            return;
        }

        ctx.matching_thread_startup.store(WorkerStartupState::READY, std::memory_order_release);

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
            for (int attempt = 0; attempt < kBoundedSpinAttempts && !stop.stop_requested();
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

            if (order.event_tag == OrderEventTag::PHASE_BARRIER)
            {
                ctx.completed_phase_barrier.store(order.order_id, std::memory_order_release);
                continue;
            }

            const uint64_t t1_ns = order.timestamp_ns;
            const uint64_t queue_residence_ns = t_pop_start_ns - t1_ns;
            const uint64_t queue_pop_ns = t_pop_end_ns - t_pop_start_ns;

            if (order.event_tag == OrderEventTag::NEW_OR_CANCEL && order.qty == 0) [[unlikely]]
            {
                // Cancels are recorded as their own sample kind.
                const uint64_t tc_start_ns = now_ns();

                // An already filled or cancelled ID is a normal no-op.
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

            // Fill callback time is measured separately from book mutation.
            uint64_t fill_publish_ns = 0;
            uint64_t fill_digest = 1469598103934665603ULL;
            const auto timed_on_fill = [&](const FillEvent &fill)
            {
                const uint64_t pub_start_ns = now_ns();
                constexpr uint64_t prime = 1099511628211ULL;
                const auto fold = [&](uint64_t value)
                {
                    fill_digest ^= value;
                    fill_digest *= prime;
                };
                fold(fill.maker_order_id);
                fold(fill.taker_order_id);
                fold(static_cast<uint64_t>(fill.price));
                fold(fill.qty);
                const uint64_t pub_end_ns = now_ns();
                fill_publish_ns += (pub_end_ns - pub_start_ns);
            };

            const bool is_replace = (order.event_tag == OrderEventTag::REPLACE);

            const uint64_t t_match_start_ns = now_ns();
            const MatchStats stats =
                is_replace ? ctx.matcher.replace(order.order_id, order.price, order.qty,
                                                 order.timestamp_ns, timed_on_fill)
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
                s.fill_digest = fill_digest;
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

} // namespace hydra
