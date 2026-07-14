// metrics/src/queue_backpressure.cpp
//
// Metric A3: SPSC queue backpressure under sustained load.
//
// Runs the real rx_thread_fn/matching_thread_fn (the actual production
// pipeline threads, not a reimplementation) for a fixed window, sampling
// the queue's occupancy from the main thread at a fixed cadence. rx_thread_fn
// generates synthetic orders with no throttling, so the queue fills and the
// matching thread's own processing rate becomes the visible bottleneck --
// this is the real, measured shape of that saturation, not a synthetic
// curve.
//
// Output: CSV to stdout -- elapsed_ms,queue_depth
// Final line to stderr: total dropped order count (scraped from
// rx_thread_fn's own backpressure counter via its stderr message).

#include "hydra/affinity.hpp"
#include "hydra/clock.hpp"
#include "hydra/pipeline.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

using namespace hydra;

int main(int argc, char **argv)
{
    const int run_seconds = (argc > 1) ? std::atoi(argv[1]) : 5;

    auto order_pool = std::make_unique<ObjectPool<Order, ORDER_POOL_SIZE>>();
    auto level_pool = std::make_unique<ObjectPool<Level, LEVEL_POOL_SIZE>>();
    auto fill_pool = std::make_unique<ObjectPool<FillEvent, FILL_EVENT_POOL_SIZE>>();
    auto book = std::make_unique<OrderBook>(*order_pool, *level_pool);
    auto matcher = std::make_unique<Matcher>(*book, *fill_pool, MatchingMode::PRICE_TIME);
    auto queue = std::make_unique<SpscQueue<Order, SPSC_CAPACITY>>();
    auto histogram = std::make_unique<HdrHistogram>();

    PipelineContext ctx{
        .queue = *queue,
        .book = *book,
        .matcher = *matcher,
        .order_pool = *order_pool,
        .level_pool = *level_pool,
        .fill_pool = *fill_pool,
        .histogram = *histogram,
        .ns_per_cycle = calibrate_ns_per_cycle(),
    };

    std::stop_source stop_source;

    std::printf("elapsed_ms,queue_depth\n");

    std::thread rx_thread(rx_thread_fn, stop_source.get_token(), std::ref(ctx));
    std::thread matching_thread(matching_thread_fn, stop_source.get_token(), std::ref(ctx));

    const auto start = std::chrono::steady_clock::now();
    constexpr auto kSampleInterval = std::chrono::milliseconds(50);
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(run_seconds))
    {
        std::this_thread::sleep_for(kSampleInterval);
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
        std::printf("%lld,%zu\n", static_cast<long long>(elapsed_ms), queue->approx_size());
        std::fflush(stdout);
    }

    stop_source.request_stop();
    rx_thread.join();
    matching_thread.join();

    return 0;
}
