//===----------------------------------------------------------------------===
// tests/test_phase7_pipeline.cpp
//
// Phase 7 exit condition (see HYDRA-LOB-Roadmap.md):
//   "two std::jthreads pin to distinct cores; verify_affinity throws
//   correctly on forced failure; T1-T4 recorded end-to-end; clean shutdown
//   on stop_token cancellation with no hangs/leaks."
//
// WHY this file links src/pipeline.cpp (see CMakeLists.txt's
// blitz_lob_test_phase7_pipeline target): unlike every other phase's file,
// rx_thread_fn/matching_thread_fn are declared in hydra/pipeline.hpp but
// defined in src/pipeline.cpp -- a real .cpp, not header-only -- so this is
// the one phase test that needs an extra source file, not just a header
// include, to actually exercise the real thread bodies rather than a
// reimplementation of them.
//===----------------------------------------------------------------------===

#include "hydra/affinity.hpp"
#include "hydra/clock.hpp"
#include "hydra/pipeline.hpp"

#include "test_harness.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace hydra::test
{
    namespace
    {

        // Core 0 always exists (unlike RX_CORE_ID/MATCHING_CORE_ID, which
        // depend on the machine having 4+ cores) -- these two tests
        // exercise affinity.hpp's own contract directly and don't need the
        // real pipeline core IDs to do it.
        void test_pin_and_verify_affinity_matching_succeeds()
        {
            pin_to_core(0);
            bool threw = false;
            try
            {
                verify_affinity(0);
            }
            catch (const std::runtime_error &)
            {
                threw = true;
            }
            HYDRA_CHECK(!threw);
        }

        void test_verify_affinity_throws_on_mismatch()
        {
            HYDRA_CHECK(std::thread::hardware_concurrency() >= 2);

            pin_to_core(0);
            bool threw = false;
            std::string message;
            try
            {
                verify_affinity(1); // deliberately wrong expected core
            }
            catch (const std::runtime_error &e)
            {
                threw = true;
                message = e.what();
            }
            HYDRA_CHECK(threw);
            HYDRA_CHECK(!message.empty());
        }

        // The core Phase 7 integration test: spawn the real rx_thread_fn
        // and matching_thread_fn as std::jthreads, pinned to the real
        // RX_CORE_ID/MATCHING_CORE_ID from config.hpp, let them run
        // briefly, then request stop and confirm they join promptly with
        // T1-T4 latency samples actually recorded end-to-end.
        void test_pipeline_threads_run_record_and_shutdown_cleanly()
        {
            if (std::thread::hardware_concurrency() <= static_cast<unsigned>(MATCHING_CORE_ID))
            {
                std::fprintf(stderr,
                             "    SKIPPED: this machine has only %u core(s), fewer than "
                             "MATCHING_CORE_ID+1 (%d) -- config.hpp's RX_CORE_ID/"
                             "MATCHING_CORE_ID don't exist here, so rx_thread_fn/"
                             "matching_thread_fn would correctly refuse to pin and exit "
                             "immediately (that failure path is exactly what "
                             "verify_affinity's own throw-on-mismatch contract, already "
                             "covered above, guards) rather than exercising the full "
                             "pipeline.\n",
                             std::thread::hardware_concurrency(), MATCHING_CORE_ID + 1);
                return;
            }

            auto order_pool = std::make_unique<ObjectPool<Order, ORDER_POOL_SIZE>>();
            auto level_pool = std::make_unique<ObjectPool<Level, LEVEL_POOL_SIZE>>();
            auto book = std::make_unique<OrderBook>(*order_pool, *level_pool);
            auto matcher = std::make_unique<Matcher>(*book, MatchingMode::PRICE_TIME);
            auto queue = std::make_unique<SpscQueue<Order, SPSC_CAPACITY>>();

            PipelineContext ctx{
                .queue = *queue,
                .book = *book,
                .matcher = *matcher,
                .order_pool = *order_pool,
                .level_pool = *level_pool,
                .ns_per_cycle = calibrate_ns_per_cycle(),
            };

            constexpr std::size_t kSinkCapacity = 200'000;
            std::vector<LatencySample> storage(kSinkCapacity);
            RawSampleSink sink;
            sink.samples = storage.data();
            sink.capacity = kSinkCapacity;
            ctx.raw_samples.store(&sink, std::memory_order_release);

            {
                std::jthread rx(rx_thread_fn, std::ref(ctx));
                std::jthread matching(matching_thread_fn, std::ref(ctx));

                std::this_thread::sleep_for(std::chrono::milliseconds(200));

                const auto stop_requested_at = std::chrono::steady_clock::now();
                rx.request_stop();
                matching.request_stop();
                rx.join();
                matching.join();
                const auto shutdown_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - stop_requested_at)
                                              .count();

                std::fprintf(stderr, "    shutdown took %.2f ms after stop request\n",
                             static_cast<double>(shutdown_ns) / 1'000'000.0);
                // Both loops check stop_requested() at least every
                // kBoundedSpinAttempts iterations of a tight spin (see
                // pipeline.cpp) -- a multi-second bound here is enormously
                // generous and exists only to fail the test instead of
                // hanging forever if shutdown ever regresses to unbounded.
                HYDRA_CHECK(shutdown_ns < 5'000'000'000LL);
            }

            ctx.raw_samples.store(nullptr, std::memory_order_release);

            const std::size_t recorded = sink.count();
            std::fprintf(stderr, "    recorded %zu latency samples in 200ms\n", recorded);
            HYDRA_CHECK(recorded > 0);

            // Every recorded sample's T1-T4-derived latencies must be sane.
            // WHY only match_time_ns gets a tight upper bound, not
            // queue_transit_ns/end_to_end_ns: rx_thread_fn pushes as fast as
            // it can spin with no throttling, so this synthetic test
            // routinely saturates the SPSC queue (confirmed above by
            // rx_thread_fn's own "dropped N order(s)" stderr line) --
            // queue_transit_ns and end_to_end_ns both legitimately grow
            // large under that real backpressure, since they include time
            // spent waiting in the queue. match_time_ns (T4-T3) never
            // includes queue wait time at all -- it is pure matching-engine
            // compute (book lookups, pointer chasing) -- so it staying small
            // even while the other two grow is both the correct invariant
            // AND an indirect regression check for the earlier per-thread-
            // independent-clock-calibration bug, which would have produced
            // spurious millisecond-to-second-scale errors here instead.
            constexpr uint64_t kSaneMatchTimeUpperBoundNs = 5'000'000; // 5ms, generous
            std::size_t non_cancel_checked = 0;
            uint64_t max_end_to_end_ns = 0;
            for (std::size_t i = 0; i < recorded; ++i)
            {
                const LatencySample &s = storage[i];
                if (s.is_cancel)
                {
                    continue;
                }
                HYDRA_CHECK(s.match_time_ns < kSaneMatchTimeUpperBoundNs);
                // Structural invariants from the underlying monotonic
                // timestamps: end-to-end always dominates both its own
                // components (queue residence + pop time together can never
                // exceed the full ingress-to-completion span).
                HYDRA_CHECK(s.end_to_end_ns >= s.match_time_ns);
                HYDRA_CHECK(s.end_to_end_ns >= s.queue_residence_ns + s.queue_pop_ns);
                if (s.end_to_end_ns > max_end_to_end_ns)
                {
                    max_end_to_end_ns = s.end_to_end_ns;
                }
                ++non_cancel_checked;
            }
            std::fprintf(stderr,
                         "    checked %zu non-cancel samples; max end_to_end_ns "
                         "observed = %llu (%.2f ms, expected to grow under this "
                         "synthetic test's sustained backpressure)\n",
                         non_cancel_checked, static_cast<unsigned long long>(max_end_to_end_ns),
                         static_cast<double>(max_end_to_end_ns) / 1'000'000.0);
            HYDRA_CHECK(non_cancel_checked > 0);
        }

        // rx_thread_fn's synthetic RNG never generates a cancel (its qty is
        // always drawn from uniform_int_distribution<uint32_t>(1,100), see
        // pipeline.cpp) -- so the test above never exercises
        // matching_thread_fn's is_cancel branch. This test pushes a
        // resting order and a matching cancel by hand, driving only
        // matching_thread_fn (no rx_thread_fn), to confirm cancels now
        // carry real, sane timestamps instead of the previous
        // unconditional LatencySample{0, 0, 0, true}.
        void test_matching_thread_records_real_cancel_latency()
        {
            if (std::thread::hardware_concurrency() <= static_cast<unsigned>(MATCHING_CORE_ID))
            {
                std::fprintf(stderr,
                             "    SKIPPED: this machine has only %u core(s), fewer than "
                             "MATCHING_CORE_ID+1 (%d) -- see the equivalent skip reason "
                             "above.\n",
                             std::thread::hardware_concurrency(), MATCHING_CORE_ID + 1);
                return;
            }

            auto order_pool = std::make_unique<ObjectPool<Order, ORDER_POOL_SIZE>>();
            auto level_pool = std::make_unique<ObjectPool<Level, LEVEL_POOL_SIZE>>();
            auto book = std::make_unique<OrderBook>(*order_pool, *level_pool);
            auto matcher = std::make_unique<Matcher>(*book, MatchingMode::PRICE_TIME);
            auto queue = std::make_unique<SpscQueue<Order, SPSC_CAPACITY>>();

            const double ns_per_cycle = calibrate_ns_per_cycle();
            PipelineContext ctx{
                .queue = *queue,
                .book = *book,
                .matcher = *matcher,
                .order_pool = *order_pool,
                .level_pool = *level_pool,
                .ns_per_cycle = ns_per_cycle,
            };

            constexpr std::size_t kSinkCapacity = 8;
            std::vector<LatencySample> storage(kSinkCapacity);
            RawSampleSink sink;
            sink.samples = storage.data();
            sink.capacity = kSinkCapacity;
            ctx.raw_samples.store(&sink, std::memory_order_release);

            {
                std::jthread matching(matching_thread_fn, std::ref(ctx));

                Order resting{};
                resting.order_id = 1;
                resting.price = 100;
                resting.qty = 10;
                resting.side = Side::BUY;
                resting.tif = TimeInForce::GTC;
                resting.client_id = 1;
                resting.timestamp_ns = static_cast<uint64_t>(
                    static_cast<double>(rdtsc_now()) * ns_per_cycle);
                HYDRA_CHECK(ctx.queue.push(resting));

                Order cancel{};
                cancel.order_id = 1;
                cancel.qty = 0; // qty==0 is the wire encoding for "cancel"
                cancel.timestamp_ns = static_cast<uint64_t>(
                    static_cast<double>(rdtsc_now()) * ns_per_cycle);
                HYDRA_CHECK(ctx.queue.push(cancel));

                while (sink.count() < 2)
                {
                    std::this_thread::yield();
                }
                matching.request_stop();
            }
            ctx.raw_samples.store(nullptr, std::memory_order_release);

            HYDRA_CHECK_EQ(sink.count(), std::size_t{2});
            const LatencySample &order_sample = storage[0];
            const LatencySample &cancel_sample = storage[1];

            HYDRA_CHECK(!order_sample.is_cancel);
            HYDRA_CHECK(cancel_sample.is_cancel);

            std::fprintf(stderr,
                         "    cancel sample: queue_residence_ns=%llu queue_pop_ns=%llu "
                         "match_time_ns=%llu end_to_end_ns=%llu\n",
                         static_cast<unsigned long long>(cancel_sample.queue_residence_ns),
                         static_cast<unsigned long long>(cancel_sample.queue_pop_ns),
                         static_cast<unsigned long long>(cancel_sample.match_time_ns),
                         static_cast<unsigned long long>(cancel_sample.end_to_end_ns));

            // The confirming assertions: previously an unconditional
            // {0, 0, 0, true} -- now real, sane, non-zero timing.
            HYDRA_CHECK(cancel_sample.end_to_end_ns > 0);
            HYDRA_CHECK(cancel_sample.end_to_end_ns >= cancel_sample.match_time_ns);
            HYDRA_CHECK(cancel_sample.end_to_end_ns >=
                        cancel_sample.queue_residence_ns + cancel_sample.queue_pop_ns);
            constexpr uint64_t kSaneCancelTimeUpperBoundNs = 5'000'000; // 5ms, generous
            HYDRA_CHECK(cancel_sample.match_time_ns < kSaneCancelTimeUpperBoundNs);
            HYDRA_CHECK(cancel_sample.match_time_ns > 0);

            // And the cancel genuinely took effect against the book, not
            // just against the timing instrumentation.
            HYDRA_CHECK(!book->best_bid().has_value());
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_pin_and_verify_affinity_matching_succeeds);
    RUN_TEST(test_verify_affinity_throws_on_mismatch);
    RUN_TEST(test_pipeline_threads_run_record_and_shutdown_cleanly);
    RUN_TEST(test_matching_thread_records_real_cancel_latency);

    return report_and_exit_code();
}
