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

    // Ablation verdict: "separated and material" on per-trial P99.9.
    //
    // Inputs are one P99.9 (ns) per trial for the optimized baseline and for the ablated build,
    // measured in the same session. Rules, first match wins:
    //   INVALID               a validity gate failed in either arm, an arm has no trials, or the
    //                         baseline median P99.9 is <= 0
    //   INCONCLUSIVE          fewer than min_trials trials in either arm (5 vs 5 is the smallest
    //                         design whose complete separation has exact one-sided p <= 0.01)
    //   NO_MEASURABLE_EFFECT  median ablated P99.9 within +/-10% of the baseline median
    //                         ("not detected", not "proven equal")
    //   PROVEN                median ablated >= 1.10x baseline AND min(ablated) > max(baseline)
    //   ABLATED_FASTER        median ablated <= baseline / 1.10 AND max(ablated) < min(baseline)
    //   INCONCLUSIVE          a material median gap whose per-trial ranges overlap
    // Always report the metric with the verdict: "PROVEN (P99.9)".
    // scripts/ablation_verdict.py implements the same rule; tests/verdict_cases.json is run
    // through both by tests/test_phase10_verdict.cpp and tests/test_verdict_parity.py.
    enum class Verdict
    {
        PROVEN,               // removing the optimization worsens P99.9 materially and consistently
        ABLATED_FASTER,       // the ablated build is materially and consistently faster
        NO_MEASURABLE_EFFECT, // medians within the materiality band
        INCONCLUSIVE,         // material median gap with overlapping trials, or too few trials
        INVALID
    };

    [[nodiscard]] constexpr const char *verdict_name(Verdict v) noexcept
    {
        switch (v)
        {
        case Verdict::PROVEN:
            return "PROVEN";
        case Verdict::ABLATED_FASTER:
            return "ABLATED_FASTER";
        case Verdict::NO_MEASURABLE_EFFECT:
            return "NO_MEASURABLE_EFFECT";
        case Verdict::INCONCLUSIVE:
            return "INCONCLUSIVE";
        case Verdict::INVALID:
            return "INVALID";
        }
        return "INVALID";
    }

    // Materiality threshold in integer percent. The comparison is done as
    // ablated * 100 >= baseline * (100 + percent) because 1.10 is not representable in binary
    // floating point and a ratio test would misclassify an exact 10% difference.
    inline constexpr double MIN_EFFECT_PERCENT = 10.0;
    inline constexpr std::size_t MIN_TRIALS = 5;

    // Validity gates of one benchmark arm (all trials of one build). Every field is true when
    // the arm is clean.
    struct RunGates
    {
        bool valid = true;
        bool determinism_ok = true;
        bool no_dropped_events = true;
        bool no_arena_fallback = true;
        bool no_pool_exhaustion = true;

        [[nodiscard]] constexpr bool all_pass() const noexcept
        {
            return valid && determinism_ok && no_dropped_events && no_arena_fallback &&
                   no_pool_exhaustion;
        }
    };

    struct AblationInput
    {
        // One P99.9 (ns) per trial; same session, same timing boundary. The ablation of the
        // flat layout legitimately replays a different dataset, so the dataset hash is not a gate.
        std::vector<double> baseline_p999_ns;
        std::vector<double> ablated_p999_ns;
        RunGates baseline;
        RunGates ablated;
        double min_effect_percent = MIN_EFFECT_PERCENT;
        std::size_t min_trials = MIN_TRIALS;
    };

    // Probability, if trials were exchangeable between arms, that all n ablated trials exceed
    // all n baseline trials: 1 / C(2n, n). 5 vs 5 -> 1/252 = 0.0040.
    [[nodiscard]] inline double complete_separation_p(std::size_t n) noexcept
    {
        double c = 1.0; // C(2n, n) = prod_{i=1..n} (n + i) / i
        for (std::size_t i = 1; i <= n; ++i)
        {
            c *= static_cast<double>(n + i) / static_cast<double>(i);
        }
        return 1.0 / c;
    }

    [[nodiscard]] inline Verdict classify_ablation(const AblationInput &in) noexcept
    {
        if (!in.baseline.all_pass() || !in.ablated.all_pass() || in.baseline_p999_ns.empty() ||
            in.ablated_p999_ns.empty() || in.min_effect_percent <= 0.0)
        {
            return Verdict::INVALID;
        }
        const double base_med = median_of(in.baseline_p999_ns);
        const double abl_med = median_of(in.ablated_p999_ns);
        if (base_med <= 0.0)
        {
            return Verdict::INVALID;
        }
        if (in.baseline_p999_ns.size() < in.min_trials || in.ablated_p999_ns.size() < in.min_trials)
        {
            return Verdict::INCONCLUSIVE;
        }

        const double up = 100.0 + in.min_effect_percent;
        const bool materially_worse = abl_med * 100.0 >= base_med * up;
        const bool materially_better = abl_med * up <= base_med * 100.0;
        if (!materially_worse && !materially_better)
        {
            return Verdict::NO_MEASURABLE_EFFECT;
        }

        const auto [b_min, b_max] =
            std::minmax_element(in.baseline_p999_ns.begin(), in.baseline_p999_ns.end());
        const auto [a_min, a_max] =
            std::minmax_element(in.ablated_p999_ns.begin(), in.ablated_p999_ns.end());
        if (materially_worse && *a_min > *b_max)
        {
            return Verdict::PROVEN;
        }
        if (materially_better && *a_max < *b_min)
        {
            return Verdict::ABLATED_FASTER;
        }
        return Verdict::INCONCLUSIVE;
    }

} // namespace hydra::stats
