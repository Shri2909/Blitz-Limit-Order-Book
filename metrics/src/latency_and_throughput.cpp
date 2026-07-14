// metrics/src/latency_and_throughput.cpp
//
// Metric B1/B2: latency distribution and throughput from a real pipeline
// run on THIS machine.
//
// IMPORTANT: unlike Phase 8's blitz_lob --benchmark, this program does NOT
// run run_preflight_checks() -- it deliberately runs on whatever hardware
// it's invoked on, tuned or not, and labels its own output accordingly so
// the report can be honest about whether these are "illustrative, this
// machine" numbers or numbers from a properly isolated benchmark box (see
// metrics/README.md and the D-category numbers, which DO go through real
// preflight). Runs the real rx_thread_fn/matching_thread_fn, attaches a
// RawSampleSink (the same mechanism benchmark.cpp itself uses), and reports
// every T1-T4 sample recorded during the window.
//
// Output: CSV to stdout -- sample_index,queue_transit_ns,match_time_ns,end_to_end_ns
// Summary line to stderr: throughput (samples/sec)

#include "hydra/affinity.hpp"
#include "hydra/clock.hpp"
#include "hydra/pipeline.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using namespace hydra;

int main(int argc, char **argv)
{
    const int run_seconds = (argc > 1) ? std::atoi(argv[1]) : 3;

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

    constexpr std::size_t kSinkCapacity = 2'000'000;
    std::vector<LatencySample> storage(kSinkCapacity);
    RawSampleSink sink;
    sink.samples = storage.data();
    sink.capacity = kSinkCapacity;
    ctx.raw_samples.store(&sink, std::memory_order_release);

    std::stop_source stop_source;
    std::thread rx_thread(rx_thread_fn, stop_source.get_token(), std::ref(ctx));
    std::thread matching_thread(matching_thread_fn, stop_source.get_token(), std::ref(ctx));

    const auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(run_seconds));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    stop_source.request_stop();
    rx_thread.join();
    matching_thread.join();
    ctx.raw_samples.store(nullptr, std::memory_order_release);

    const std::size_t recorded = sink.count();
    const double elapsed_s =
        std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();

    std::printf("sample_index,queue_transit_ns,match_time_ns,end_to_end_ns\n");
    std::size_t non_cancel = 0;
    for (std::size_t i = 0; i < recorded; ++i)
    {
        const LatencySample &s = storage[i];
        if (s.is_cancel)
        {
            continue;
        }
        std::printf("%zu,%llu,%llu,%llu\n", non_cancel,
                    static_cast<unsigned long long>(s.queue_transit_ns),
                    static_cast<unsigned long long>(s.match_time_ns),
                    static_cast<unsigned long long>(s.end_to_end_ns));
        ++non_cancel;
    }

    std::fprintf(stderr, "throughput_samples_per_sec=%.1f\nelapsed_s=%.3f\nsample_count=%zu\n",
                 static_cast<double>(non_cancel) / elapsed_s, elapsed_s, non_cancel);

    return 0;
}
