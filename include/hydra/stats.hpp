#pragma once

// Statistics and ablation verdicts for benchmark reports.
// See METHODOLOGY.md for definitions and comparison thresholds.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace hydra::stats
{

    // Rank-index percentile on sorted samples: floor(p / 100 * N), clamped
    // to N - 1, without interpolation.
    [[nodiscard]] inline double percentile(const std::vector<uint64_t> &sorted_values,
                                           double p) noexcept
    {
        if (sorted_values.empty())
        {
            return 0.0;
        }
        std::size_t idx =
            static_cast<std::size_t>((p / 100.0) * static_cast<double>(sorted_values.size()));
        if (idx >= sorted_values.size())
        {
            idx = sorted_values.size() - 1;
        }
        return static_cast<double>(sorted_values[idx]);
    }

    struct PercentileSet
    {
        double p50 = 0.0;
        double p90 = 0.0;
        double p99 = 0.0;
        double p999 = 0.0;
        double max = 0.0;
        double mean = 0.0;
    };

    // Compute report percentiles and the mean from ascending samples.
    [[nodiscard]] inline PercentileSet
    compute_percentiles(const std::vector<uint64_t> &sorted_values) noexcept
    {
        PercentileSet r{};
        if (sorted_values.empty())
        {
            return r;
        }
        r.p50 = percentile(sorted_values, 50.0);
        r.p90 = percentile(sorted_values, 90.0);
        r.p99 = percentile(sorted_values, 99.0);
        r.p999 = percentile(sorted_values, 99.9);
        r.max = static_cast<double>(sorted_values.back());
        double sum = 0.0;
        for (const uint64_t v : sorted_values)
        {
            sum += static_cast<double>(v);
        }
        r.mean = sum / static_cast<double>(sorted_values.size());
        return r;
    }

    [[nodiscard]] inline double mean(const std::vector<double> &v) noexcept
    {
        if (v.empty())
        {
            return 0.0;
        }
        double sum = 0.0;
        for (const double x : v)
        {
            sum += x;
        }
        return sum / static_cast<double>(v.size());
    }

    // Sample standard deviation (n - 1 denominator); zero for fewer than two samples.
    [[nodiscard]] inline double stddev(const std::vector<double> &v) noexcept
    {
        if (v.size() < 2)
        {
            return 0.0;
        }
        const double m = mean(v);
        double sq_sum = 0.0;
        for (const double x : v)
        {
            sq_sum += (x - m) * (x - m);
        }
        return std::sqrt(sq_sum / static_cast<double>(v.size() - 1));
    }

    // Median of a copy; average the two middle values for an even sample count.
    [[nodiscard]] inline double median_of(std::vector<double> v) noexcept
    {
        if (v.empty())
        {
            return 0.0;
        }
        std::sort(v.begin(), v.end());
        const std::size_t n = v.size();
        return (n % 2 == 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
    }

    [[nodiscard]] inline double coefficient_of_variation(const std::vector<double> &v) noexcept
    {
        const double m = mean(v);
        if (m == 0.0)
        {
            return 0.0;
        }
        return stddev(v) / m;
    }

    [[nodiscard]] inline double median_across_trials(std::vector<double> v) noexcept
    {
        return median_of(std::move(v));
    }

    enum class Verdict
    {
        PROVEN,
        INCONCLUSIVE,
        NOT_PROVEN,
        REGRESSION,
        INVALID
    };

    // Compare one ablated variant with the optimized baseline. Positive
    // latency deltas (handicapped - optimized) favor the optimization.
    struct AblationInput
    {
        double optimized_p99_ns = 0.0;
        double optimized_p99_9_ns = 0.0;
        double handicapped_p99_ns = 0.0;
        double handicapped_p99_9_ns = 0.0;
        // Per-trial P99 coefficients of variation determine the noise floor.
        double optimized_cv = 0.0;
        double handicapped_cv = 0.0;
        uint64_t optimized_dropped_events = 0;
        uint64_t handicapped_dropped_events = 0;
        // Require passing correctness checks and equivalent datasets, builds,
        // timing boundaries, and CPU/NUMA policies for both runs.
        bool correctness_ok = true;
        bool same_environment = true;
    };

    // Invalid inputs take precedence. Regression requires the ablated build
    // to be faster at both tail percentiles. Proven improvement requires the
    // P99 delta to exceed the CV-based noise floor and a positive P99.9 delta.
    [[nodiscard]] inline Verdict classify_ablation(const AblationInput &in) noexcept
    {
        if (!in.correctness_ok || !in.same_environment || in.optimized_dropped_events != 0 ||
            in.handicapped_dropped_events != 0)
        {
            return Verdict::INVALID;
        }

        const double delta_p99 = in.handicapped_p99_ns - in.optimized_p99_ns;
        const double delta_p99_9 = in.handicapped_p99_9_ns - in.optimized_p99_9_ns;

        if (delta_p99 < 0.0 && delta_p99_9 < 0.0)
        {
            return Verdict::REGRESSION;
        }

        const double noise_floor_cv = std::max(in.optimized_cv, in.handicapped_cv);
        const double noise_floor_ns = noise_floor_cv * in.optimized_p99_ns;

        if (delta_p99 > noise_floor_ns && delta_p99_9 > 0.0)
        {
            return Verdict::PROVEN;
        }
        if (delta_p99 <= 0.0 && delta_p99_9 <= 0.0)
        {
            return Verdict::NOT_PROVEN;
        }
        return Verdict::INCONCLUSIVE;
    }

} // namespace hydra::stats
