// Clock monotonicity and calibration tests (calibration takes about two seconds).

#include "hydra/clock.hpp"

#include "test_harness.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

namespace hydra::test
{
    namespace
    {

        void test_rdtsc_now_monotonic()
        {
            uint64_t previous = rdtsc_now();
            for (int i = 0; i < 1000; ++i)
            {
                const uint64_t current = rdtsc_now();
                HYDRA_CHECK(current >= previous);
                previous = current;
            }
        }

        // A 1 ms sleep, converted with the calibrated ratio, lands in a plausible range.
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

            // Allow scheduler overshoot but reject implausible values.
            HYDRA_CHECK(measured_ns >= 1'000'000.0);
            HYDRA_CHECK(measured_ns <= 50'000'000.0);
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_rdtsc_now_monotonic);
    RUN_TEST(test_clock_calibration_and_measurement);

    return report_and_exit_code();
}
