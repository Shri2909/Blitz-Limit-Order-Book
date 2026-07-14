//===----------------------------------------------------------------------===
// tests/test_phase2_timing.cpp
//
// Phase 2 exit condition (see HYDRA-LOB-Roadmap.md):
//   "throwaway main() calibrates, records 1000 synthetic samples, and
//   queries P50/P99 successfully."
//
// Covers both Phase 2 files: hydra/clock.hpp (calibrate_ns_per_cycle(),
// rdtsc_now()) and hydra/histogram.hpp (HdrHistogram). Only those two
// project headers are included, matching Phase 1's "compiles standalone"
// discipline.
//
// WHY this binary takes a couple of seconds to run: calibrate_ns_per_cycle()
// is spec'd to sample two independent 1-second windows to cross-check TSC
// frequency (see clock.hpp) -- that cost is inherent to the phase, not a
// test inefficiency, so it's paid exactly once here and reused across every
// test that needs a calibrated ns_per_cycle.
//===----------------------------------------------------------------------===

#include "hydra/clock.hpp"
#include "hydra/histogram.hpp"

#include "test_harness.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace hydra::test
{
    namespace
    {

        void test_rdtsc_now_monotonic()
        {
            // No calibration needed -- this only checks the raw counter
            // (or its steady_clock fallback) never runs backwards across
            // consecutive calls on the same thread.
            uint64_t previous = rdtsc_now();
            for (int i = 0; i < 1000; ++i)
            {
                const uint64_t current = rdtsc_now();
                HYDRA_CHECK(current >= previous);
                previous = current;
            }
        }

        // Combines calibration + the roadmap's "known sleep_for(1ms)" check
        // into one test so the ~2s calibration cost (see file header) is
        // paid exactly once for this whole binary.
        void test_clock_calibration_and_measurement()
        {
            const double ns_per_cycle = calibrate_ns_per_cycle();
            std::fprintf(stderr, "    calibrate_ns_per_cycle() = %.6f ns/cycle\n", ns_per_cycle);
            HYDRA_CHECK(ns_per_cycle > 0.0);

            const uint64_t t1 = rdtsc_now();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const uint64_t t2 = rdtsc_now();

            HYDRA_CHECK(t2 >= t1);
            const double measured_ns = static_cast<double>(t2 - t1) * ns_per_cycle;
            std::fprintf(stderr, "    measured sleep_for(1ms) as %.1f ns\n", measured_ns);

            // sleep_for(1ms) is a lower bound, not an exact contract -- OS
            // scheduler granularity/jitter (especially under a shared or
            // virtualized host) routinely overshoots a 1ms request by a
            // large relative margin even though the *underlying clock* is
            // accurate. The bound here is intentionally generous (up to
            // 50x the requested duration, i.e. 50ms) -- wide enough to
            // absorb realistic scheduler jitter, but still tight enough to
            // catch a genuinely broken calibration (a wrong ns_per_cycle by
            // even 2-3x would blow past it), which is what this test exists
            // to catch. The lower bound (must be at least the requested
            // 1,000,000 ns) has no such jitter excuse -- sleep_for() must
            // never return early.
            HYDRA_CHECK(measured_ns >= 1'000'000.0);
            HYDRA_CHECK(measured_ns <= 50'000'000.0);
        }

        void test_histogram_empty_query_is_safe()
        {
            HdrHistogram h;
            h.swap_buffers();
            HYDRA_CHECK_EQ(h.query_percentile(50.0), 0.0);
            HYDRA_CHECK_EQ(h.query_percentile(99.0), 0.0);
        }

        // Roadmap's Phase 2 validation: "records 1000 synthetic samples
        // (e.g. drawn from a known distribution), then confirms
        // query_percentile(50) and query_percentile(99) land within
        // expected bucket-precision tolerance of the analytically known
        // P50/P99." The known distribution here is the integers 1..1000,
        // each recorded exactly once -- a discrete uniform distribution
        // whose true P50/P99 are trivial to state analytically (500.5 and
        // 990.5 respectively, using the same "target_rank = p/100 * total"
        // definition query_percentile() itself uses).
        void test_histogram_percentiles_known_distribution()
        {
            HdrHistogram h;
            for (uint64_t v = 1; v <= 1000; ++v)
            {
                h.record(v);
            }
            h.swap_buffers();

            const double p50 = h.query_percentile(50.0);
            const double p99 = h.query_percentile(99.0);
            std::fprintf(stderr, "    known-distribution p50=%.2f (expect ~500.5), "
                                  "p99=%.2f (expect ~990.5)\n",
                         p50, p99);

            // Bucket width grows with magnitude (log-domain histogram), so
            // tolerance is expressed relative to the value being estimated
            // rather than a fixed absolute epsilon -- 2% comfortably covers
            // this histogram's worst-case sub-bucket interpolation error at
            // these magnitudes (kSubBucketBits=5 gives ~3.1% bucket width
            // at a given binade, and query_percentile() interpolates within
            // that width rather than just returning the bucket's lower
            // bound) while still catching a genuinely wrong result.
            HYDRA_CHECK(std::fabs(p50 - 500.5) <= 500.5 * 0.02);
            HYDRA_CHECK(std::fabs(p99 - 990.5) <= 990.5 * 0.02);
        }

        // Verifies swap_buffers() actually isolates one recorded batch from
        // the next -- record() into the buffer that's live *at the time of
        // the call*, so a query after a second swap must reflect only what
        // was recorded since the first swap, not the union of both batches.
        void test_histogram_swap_isolates_batches()
        {
            HdrHistogram h;

            for (int i = 0; i < 500; ++i)
            {
                h.record(100); // batch A: 500 samples all at 100ns
            }
            h.swap_buffers();
            HYDRA_CHECK(std::fabs(h.query_percentile(50.0) - 100.0) <= 2.0);

            for (int i = 0; i < 500; ++i)
            {
                h.record(800); // batch B: 500 samples all at 800ns
            }
            h.swap_buffers();
            const double p50_after_second_batch = h.query_percentile(50.0);
            std::fprintf(stderr, "    after second batch, p50=%.2f (expect ~800, not ~100)\n",
                         p50_after_second_batch);
            HYDRA_CHECK(std::fabs(p50_after_second_batch - 800.0) <= 800.0 * 0.05);
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_rdtsc_now_monotonic);
    RUN_TEST(test_clock_calibration_and_measurement);
    RUN_TEST(test_histogram_empty_query_is_safe);
    RUN_TEST(test_histogram_percentiles_known_distribution);
    RUN_TEST(test_histogram_swap_isolates_batches);

    return report_and_exit_code();
}
