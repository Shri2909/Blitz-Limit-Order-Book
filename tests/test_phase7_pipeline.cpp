// Thread affinity, pipeline shutdown and latency-sample tests.

#include "hydra/affinity.hpp"
#include "hydra/clock.hpp"
#include "hydra/pipeline.hpp"

#include "test_fixtures.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace hydra::test
{
    namespace
    {

        // The pipeline cores may not exist on this host, so these tests skip without them.
        [[nodiscard]] bool pipeline_cores_available()
        {
            if (std::thread::hardware_concurrency() > static_cast<unsigned>(MATCHING_CORE_ID))
            {
                return true;
            }
            std::fprintf(stderr, "    SKIPPED: fewer than %d cores available\n",
                         MATCHING_CORE_ID + 1);
            return false;
        }

        // Pinning to core 0 does not need the configured pipeline cores.
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

        // The RX and matching threads shut down promptly and record sane latency samples.
        void test_pipeline_threads_run_record_and_shutdown_cleanly()
        {
            if (!pipeline_cores_available())
            {
                return;
            }

            Fixture f;
            auto queue = std::make_unique<SpscQueue<Order, SPSC_CAPACITY>>();

            PipelineContext ctx{
                .queue = *queue,
                .book = *f.book,
                .matcher = *f.matcher,
                .order_pool = *f.order_pool,
                .level_pool = *f.level_pool,
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
                // Bound shutdown time so a stalled worker cannot hang the test.
                HYDRA_CHECK(shutdown_ns < 5'000'000'000LL);
            }

            ctx.raw_samples.store(nullptr, std::memory_order_release);

            const std::size_t recorded = sink.count();
            std::fprintf(stderr, "    recorded %zu latency samples in 200ms\n", recorded);
            HYDRA_CHECK(recorded > 0);

            // Queue and end-to-end latency grow under backpressure, so bound match time alone.
            constexpr uint64_t kSaneMatchTimeUpperBoundNs = 5'000'000;
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
                // End-to-end latency covers each component.
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

        // A cancel pushed straight onto the queue (the RX generator emits only new orders)
        // produces a valid latency sample and removes the resting order.
        void test_matching_thread_records_real_cancel_latency()
        {
            if (!pipeline_cores_available())
            {
                return;
            }

            Fixture f;
            auto queue = std::make_unique<SpscQueue<Order, SPSC_CAPACITY>>();

            const double ns_per_cycle = calibrate_ns_per_cycle();
            PipelineContext ctx{
                .queue = *queue,
                .book = *f.book,
                .matcher = *f.matcher,
                .order_pool = *f.order_pool,
                .level_pool = *f.level_pool,
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

                Order resting = make_order(1, 100, 10, Side::BUY);
                resting.timestamp_ns =
                    static_cast<uint64_t>(static_cast<double>(rdtsc_now()) * ns_per_cycle);
                HYDRA_CHECK(ctx.queue.push(resting));

                Order cancel{};
                cancel.order_id = 1;
                cancel.qty = 0; // qty == 0 encodes a cancel
                cancel.timestamp_ns =
                    static_cast<uint64_t>(static_cast<double>(rdtsc_now()) * ns_per_cycle);
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

            HYDRA_CHECK(cancel_sample.end_to_end_ns > 0);
            HYDRA_CHECK(cancel_sample.end_to_end_ns >= cancel_sample.match_time_ns);
            HYDRA_CHECK(cancel_sample.end_to_end_ns >=
                        cancel_sample.queue_residence_ns + cancel_sample.queue_pop_ns);
            constexpr uint64_t kSaneCancelTimeUpperBoundNs = 5'000'000;
            HYDRA_CHECK(cancel_sample.match_time_ns < kSaneCancelTimeUpperBoundNs);
            HYDRA_CHECK(cancel_sample.match_time_ns > 0);

            // The cancelled order is gone from the book.
            HYDRA_CHECK(!f.book->best_bid().has_value());
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
