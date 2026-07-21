#pragma once

// include/hydra/clock.hpp
//
// Cycle-accurate, serialized timestamping for latency measurement.

#if !defined(__x86_64__) && !defined(_M_X64)
#error "clock.hpp: RDTSC/RDTSCP timing requires an x86-64 target"
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <x86intrin.h> // __rdtscp, _mm_lfence

namespace hydra
{

    namespace detail
    {

        inline std::atomic<bool> g_use_fallback_clock{false};

        // WHY the _mm_lfence() pair: RDTSC/RDTSCP is not serializing on its
        // own -- out-of-order execution can hoist or sink the actual
        // timestamp read relative to the code being measured, silently
        // corrupting latency numbers at the nanosecond scale this system
        // cares about. The lfence before and after forces all prior
        // instructions to retire (and blocks speculative execution of later
        // ones) before/after the TSC read, so the measured window actually
        // brackets the code under test instead of an out-of-order-shuffled
        // approximation of it.
        [[nodiscard]] inline uint64_t serialized_rdtscp(unsigned &aux) noexcept
        {
            _mm_lfence();
            const uint64_t t = __rdtscp(&aux);
            _mm_lfence();
            return t;
        }

    } // namespace detail

    [[nodiscard]] inline double calibrate_ns_per_cycle() noexcept
    {
        constexpr double kDisagreementTolerance = 0.005;

        auto measure_once = []() noexcept -> std::pair<double, bool>
        {
            unsigned aux_start = 0, aux_end = 0;

            const uint64_t cycles_start = detail::serialized_rdtscp(aux_start);
            const auto time_start = std::chrono::steady_clock::now();

            std::this_thread::sleep_for(std::chrono::seconds(1));

            const auto time_end = std::chrono::steady_clock::now();
            const uint64_t cycles_end = detail::serialized_rdtscp(aux_end);

            const bool migrated = (aux_start != aux_end);
            const uint64_t elapsed_cycles = cycles_end - cycles_start;
            const double elapsed_ns = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(time_end - time_start).count());

            const double ns_per_cycle =
                (elapsed_cycles == 0) ? 0.0 : (elapsed_ns / static_cast<double>(elapsed_cycles));
            return {ns_per_cycle, migrated};
        };

        const auto [estimate_a, migrated_a] = measure_once();
        const auto [estimate_b, migrated_b] = measure_once();

        const double mean_estimate = (estimate_a + estimate_b) / 2.0;
        const double relative_diff =
            (mean_estimate == 0.0) ? 1.0 : std::fabs(estimate_a - estimate_b) / mean_estimate;

        const bool unreliable =
            migrated_a || migrated_b || mean_estimate <= 0.0 || relative_diff > kDisagreementTolerance;

        if (unreliable)
        {
            std::fprintf(stderr,
                         "[hydra::clock] WARNING: TSC calibration unreliable "
                         "(estimate_a=%.6f ns/cycle, estimate_b=%.6f ns/cycle, "
                         "relative_diff=%.4f%%, migrated=%s) -- falling back to "
                         "a steady_clock-based rdtsc_now() shim.\n",
                         estimate_a, estimate_b, relative_diff * 100.0,
                         (migrated_a || migrated_b) ? "yes" : "no");
            detail::g_use_fallback_clock.store(true, std::memory_order_relaxed);
            return 1.0;
        }

        return mean_estimate;
    }

    [[nodiscard]] inline uint64_t rdtsc_now() noexcept
    {
        if (detail::g_use_fallback_clock.load(std::memory_order_relaxed)) [[unlikely]]
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
        }

        unsigned aux;
        (void)aux;
        return detail::serialized_rdtscp(aux);
    }

} // namespace hydra
