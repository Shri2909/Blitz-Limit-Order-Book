#pragma once

#include <atomic>
#include <cstddef>
#include <stop_token>

#include "hydra/config.hpp"
#include "hydra/matcher.hpp"
#include "hydra/object_pool.hpp"
#include "hydra/order_book.hpp"
#include "hydra/spsc_queue.hpp"
#include "hydra/types.hpp"

namespace hydra
{

    // Per-event timing boundaries and matching counters.
    // See METHODOLOGY.md for measurement definitions.
    struct LatencySample
    {
        // Pop-call start minus the producer timestamp. Includes the successful
        // push() call, whose duration is reported separately as a trial aggregate.
        uint64_t queue_residence_ns;
        // Time inside the one successful SpscQueue::pop() call itself.
        uint64_t queue_pop_ns;
        // Matching or replacement time, excluding fill_publish_ns. For cancels,
        // the duration of OrderBook::cancel_order().
        uint64_t match_time_ns;
        // Total synchronous fill-callback duration, zero when no fills occur.
        // Timing adds one serialized RDTSCP pair per fill; see clock overhead.
        uint64_t fill_publish_ns;
        uint64_t end_to_end_ns;
        // FNV-1a fold of each fill's maker, taker, price, and quantity.
        uint64_t fill_digest = 0;
        uint32_t fills_generated = 0;
        uint32_t levels_consumed = 0;
        uint32_t resting_orders_examined = 0;
        uint32_t eligible_orders_examined = 0;
        uint32_t self_trade_skips = 0;
        uint32_t remaining_qty = 0;
        bool is_cancel = false;
        bool is_replace = false;
    };

    struct RawSampleSink
    {
        LatencySample *samples = nullptr;
        std::size_t capacity = 0;
        std::atomic<std::size_t> write_index{0};

        void record(LatencySample s) noexcept
        {
            const std::size_t idx = write_index.load(std::memory_order_relaxed);
            if (idx >= capacity) [[unlikely]]
            {
                return;
            }
            samples[idx] = s;
            write_index.store(idx + 1, std::memory_order_release);
        }

        [[nodiscard]] std::size_t count() const noexcept
        {
            return write_index.load(std::memory_order_acquire);
        }

        void reset() noexcept { write_index.store(0, std::memory_order_relaxed); }
    };

    struct PipelineContext
    {
        SpscQueue<Order, SPSC_CAPACITY> &queue;
        OrderBook &book;
        Matcher &matcher;
        ObjectPool<Order, ORDER_POOL_SIZE> &order_pool;
        ObjectPool<Level, LEVEL_POOL_SIZE> &level_pool;

        // Calibrate once before starting threads and share the scale across all
        // timestamp conversions. Independent estimates would amplify calibration
        // noise when timestamps from different threads are subtracted.
        double ns_per_cycle = 0.0;

        std::atomic<RawSampleSink *> raw_samples{nullptr};
    };

    void rx_thread_fn(std::stop_token stop, PipelineContext &ctx);

    void matching_thread_fn(std::stop_token stop, PipelineContext &ctx);

} // namespace hydra
