#pragma once

// include/hydra/stats.hpp
//
// Pure, header-only statistics functions used by the benchmark harness
// (src/benchmark.cpp) so percentile/spread/verdict logic lives in exactly
// one tested place. classify_ablation() below is also the reference this
// project's own methodology notes says scripts/run_ablations.sh's embedded
// Python verdict logic is ported from -- a dedicated compiled ablation-report
// tool was scoped but deferred, so that Python port is the only other
// consumer of this same logic today, not this header directly. See
// METHODOLOGY.md for the precise definition of every statistic below and
// the reasoning behind the verdict thresholds.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace hydra::stats
{

    // Nearest-rank percentile over an already-sorted (ascending) vector:
    // idx = floor(p/100 * N), clamped to N-1. Deterministic, no
    // interpolation between adjacent ranks -- kept from the original
    // implementation deliberately (a legitimate, precisely-named method),
    // not replaced with linear interpolation, so historical CSVs and this
    // one use the same definition of "P99".
    [[nodiscard]] inline double percentile(const std::vector<uint64_t> &sorted_values, double p) noexcept
    {
        if (sorted_values.empty())
        {
            return 0.0;
        }
        std::size_t idx = static_cast<std::size_t>((p / 100.0) * static_cast<double>(sorted_values.size()));
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

    // Computes every percentile the canonical report requires from one
    // already-sorted (ascending) sample vector, in one pass over the
    // vector for the mean plus one percentile() call per rank -- avoids
    // every caller re-deriving its own subset (the original harness only
    // ever computed P99/P99.9, silently never surfacing P50/P90/max/mean).
    [[nodiscard]] inline PercentileSet compute_percentiles(const std::vector<uint64_t> &sorted_values) noexcept
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

    // Sample standard deviation (n-1 denominator). Returns 0.0 for n < 2 --
    // a single sample has no defined spread, and reporting NaN there would
    // be worse than reporting an honest zero with the sample count printed
    // alongside it (every call site in benchmark.cpp/blitz_ablation_report
    // also reports the sample count).
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

    // Median absolute deviation: median(|x_i - median(x)|). A robust
    // spread statistic that, unlike stddev, is not dominated by a single
    // outlier trial.
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

    [[nodiscard]] inline double mad(const std::vector<double> &v) noexcept
    {
        if (v.empty())
        {
            return 0.0;
        }
        const double med = median_of(v);
        std::vector<double> deviations;
        deviations.reserve(v.size());
        for (const double x : v)
        {
            deviations.push_back(std::fabs(x - med));
        }
        return median_of(deviations);
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

    // "Median across trials," used for the canonical median-of-N-trials
    // figures: sorts a copy and averages the two middle elements for an
    // even trial count, returns the single middle element for an odd one
    // (DEFAULT_TRIAL_COUNT=5 is odd, so this is a true median at the
    // canonical trial count, not an approximation).
    [[nodiscard]] inline double median_across_trials(std::vector<double> v) noexcept
    {
        return median_of(std::move(v));
    }

    struct ConfidenceInterval
    {
        double lower = 0.0;
        double upper = 0.0;
        // False when the sample count is too small for the interval to be
        // defensible (see docs/BENCHMARK_METHODOLOGY.md) -- callers must
        // print "not defensible, n too small" rather than the numeric
        // bounds when this is false.
        bool defensible = false;
    };

    // Normal approximation (z=1.96) 95% confidence interval on the mean.
    // Only considered defensible at n >= 5 (DEFAULT_TRIAL_COUNT) -- this is
    // still an approximation (a t-distribution interval would be more
    // correct at small n), disclosed as such rather than presented as a
    // rigorous small-sample interval.
    [[nodiscard]] inline ConfidenceInterval confidence_interval_95(const std::vector<double> &v) noexcept
    {
        ConfidenceInterval ci{};
        if (v.size() < 5)
        {
            ci.defensible = false;
            return ci;
        }
        const double m = mean(v);
        const double sd = stddev(v);
        const double se = sd / std::sqrt(static_cast<double>(v.size()));
        ci.lower = m - 1.96 * se;
        ci.upper = m + 1.96 * se;
        ci.defensible = true;
        return ci;
    }

    enum class Verdict
    {
        PROVEN,
        INCONCLUSIVE,
        NOT_PROVEN,
        REGRESSION,
        INVALID
    };

    [[nodiscard]] inline const char *verdict_name(Verdict v) noexcept
    {
        switch (v)
        {
        case Verdict::PROVEN:
            return "PROVEN";
        case Verdict::INCONCLUSIVE:
            return "INCONCLUSIVE";
        case Verdict::NOT_PROVEN:
            return "NOT PROVEN";
        case Verdict::REGRESSION:
            return "REGRESSION";
        case Verdict::INVALID:
            return "INVALID";
        }
        return "INVALID";
    }

    // Everything classify_ablation() needs to compare one ablated variant
    // against the fully-optimized baseline for a single matching policy.
    // "optimized" == the production/fully-optimized build; "handicapped"
    // == the ablated variant with one optimization disabled. A positive
    // p99/p99_9 delta (handicapped - optimized) means the optimization
    // helps -- matching the Delta Rules' "positive latency improvement
    // must mean the optimized implementation is faster" requirement.
    struct AblationInput
    {
        double optimized_p99_ns = 0.0;
        double optimized_p99_9_ns = 0.0;
        double handicapped_p99_ns = 0.0;
        double handicapped_p99_9_ns = 0.0;
        // Coefficient of variation of each side's own per-trial P99 series
        // -- the noise floor a delta must clear to be called PROVEN.
        double optimized_cv = 0.0;
        double handicapped_cv = 0.0;
        uint64_t optimized_dropped_events = 0;
        uint64_t handicapped_dropped_events = 0;
        // False if either side failed a correctness/reference/Pro-Rata-
        // invariant/determinism check, or if any equivalence condition
        // (commit, dataset, compiler flags, timing boundaries, CPU/NUMA
        // policy, correctness checks) did not hold between the two runs.
        bool correctness_ok = true;
        bool same_environment = true;
    };

    // INVALID takes priority over every other verdict: a comparison whose
    // preconditions don't hold cannot prove or disprove anything, no
    // matter how large the observed delta is. REGRESSION requires the
    // ablated ("handicapped") variant to be faster on BOTH P99 and P99.9 --
    // a mixed result (faster on one, slower on the other) falls through to
    // INCONCLUSIVE/NOT PROVEN instead, since a real regression should show
    // up consistently across both percentiles. PROVEN requires the delta
    // to clear a noise floor derived from each side's own trial-to-trial
    // CV (not just "trial 1 was faster") on both P99 and (a nonzero
    // improvement on) P99.9.
    [[nodiscard]] inline Verdict classify_ablation(const AblationInput &in) noexcept
    {
        if (!in.correctness_ok || !in.same_environment ||
            in.optimized_dropped_events != 0 || in.handicapped_dropped_events != 0)
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

    // Delta Rules (spec-mandated formulas, kept as named functions rather
    // than inlined ad hoc at each call site so the sign convention -- a
    // positive value always means the optimized build is better -- can't
    // silently flip between callers).
    [[nodiscard]] inline double absolute_latency_improvement_ns(double handicapped_ns,
                                                                 double optimized_ns) noexcept
    {
        return handicapped_ns - optimized_ns;
    }

    [[nodiscard]] inline double percentage_latency_reduction(double handicapped_ns,
                                                              double optimized_ns) noexcept
    {
        if (handicapped_ns == 0.0)
        {
            return 0.0;
        }
        return (handicapped_ns - optimized_ns) / handicapped_ns * 100.0;
    }

    [[nodiscard]] inline double absolute_throughput_improvement(double optimized_ops,
                                                                 double handicapped_ops) noexcept
    {
        return optimized_ops - handicapped_ops;
    }

    [[nodiscard]] inline double percentage_throughput_increase(double optimized_ops,
                                                                double handicapped_ops) noexcept
    {
        if (handicapped_ops == 0.0)
        {
            return 0.0;
        }
        return (optimized_ops - handicapped_ops) / handicapped_ops * 100.0;
    }

} // namespace hydra::stats
