#pragma once

#include <atomic>
#include <cstddef>
#include <stop_token>
#include <string>

#include "hydra/config.hpp"
#include "hydra/matcher.hpp"
#include "hydra/object_pool.hpp"
#include "hydra/order_book.hpp"
#include "hydra/spsc_queue.hpp"
#include "hydra/types.hpp"

#ifdef ENABLE_AFXDP
#include "hydra/xdp/xdp_socket.hpp"
#endif

namespace hydra
{

    // Explicit measurement-category breakdown (see
    // docs/BENCHMARK_METHODOLOGY.md for the exact start/end boundary of
    // every field below). Replaces the old single fused "queue_transit_ns"
    // (pop-done doubled as match-start) with a real, separately-timed
    // pop boundary, plus per-order MatchStats so the CSV/report layer never
    // has to approximate "levels consumed"/"orders examined" from
    // externally-observable fill events.
    struct LatencySample
    {
        // t_pop_call_start - order.timestamp_ns. Approximate, disclosed:
        // includes the producer's own (typically single-digit-ns, measured
        // separately as its own aggregate -- see ReplayPhaseResult in
        // benchmark.cpp) push() call duration, since that duration cannot
        // be attributed back into the transported Order value without
        // knowing it before the value is copied into the queue. See
        // docs/BENCHMARK_METHODOLOGY.md.
        uint64_t queue_residence_ns;
        // Time inside the one successful SpscQueue::pop() call itself.
        uint64_t queue_pop_ns;
        // For an order/replace sample: time inside Matcher::match()/
        // Matcher::replace(), EXCLUDING fill_publish_ns below (the on_fill
        // callback is bracketed separately so book-mutation time and
        // fill-delivery time are cleanly partitioned, not fused). For a
        // cancel sample: time inside OrderBook::cancel_order() -- same
        // "time to perform the operation, queue wait excluded" concept
        // either way, which is why this is one field disambiguated by
        // is_cancel/is_replace rather than several.
        uint64_t match_time_ns;
        // Cumulative time inside the on_fill() callback across every fill
        // this order generated (0 if fills_generated == 0). Always
        // measured (not flag-gated): the added cost is one serialized
        // RDTSCP pair per generated fill, paid only on orders that actually
        // cross, and disclosed via the report's "Clock overhead" field.
        uint64_t fill_publish_ns;
        uint64_t end_to_end_ns;
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

        void reset() noexcept
        {
            write_index.store(0, std::memory_order_relaxed);
        }
    };

    struct PipelineContext
    {
        SpscQueue<Order, SPSC_CAPACITY> &queue;
        OrderBook &book;
        Matcher &matcher;
        ObjectPool<Order, ORDER_POOL_SIZE> &order_pool;
        ObjectPool<Level, LEVEL_POOL_SIZE> &level_pool;

        // Calibrated ONCE (see calibrate_ns_per_cycle() in clock.hpp) by
        // whoever constructs the pipeline, before any thread starts, and
        // shared by every thread that converts rdtsc_now() to nanoseconds.
        // rx_thread_fn and matching_thread_fn read the *same* physical TSC;
        // if each thread instead calibrated its own scale factor
        // independently, the two estimates would differ by ordinary
        // measurement noise, and subtracting a timestamp computed with one
        // thread's scale from a timestamp computed with the other thread's
        // scale (queue_transit_ns = t2_ns - t1_ns) would multiply that noise
        // by the raw TSC value (which grows with machine uptime), producing
        // spurious millisecond-scale errors on nanosecond-scale
        // measurements.
        double ns_per_cycle = 0.0;

        std::atomic<RawSampleSink *> raw_samples{nullptr};
    };

    void rx_thread_fn(std::stop_token stop, PipelineContext &ctx);

    void matching_thread_fn(std::stop_token stop, PipelineContext &ctx);

#ifdef ENABLE_AFXDP

    // Real-traffic alternative to rx_thread_fn's synthetic order generator:
    // drains an AF_XDP socket bound to {ifname, queue_id} instead of an RNG.
    // See src/pipeline.cpp's afxdp_rx_thread_fn for the loop itself.
    struct AfxdpConfig
    {
        std::string ifname;
        uint32_t queue_id = config::AFXDP_DEFAULT_QUEUE_ID;
        std::string bpf_prog_path;
        // Global type from <xdp/libxdp.h> (pulled in transitively via
        // hydra/xdp/xdp_socket.hpp above) -- NOT hydra::xdp::xdp_attach_mode,
        // there is no such nested type; xdp_socket.hpp's own free functions
        // (load_and_attach_xdp_program, detach_xdp_program) take it
        // unqualified for the same reason.
        xdp_attach_mode attach_mode = XDP_MODE_SKB;
    };

    void afxdp_rx_thread_fn(std::stop_token stop, PipelineContext &ctx, const AfxdpConfig &cfg);

#endif // ENABLE_AFXDP

} // namespace hydra