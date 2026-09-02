#pragma once

// new-feature/include/asmm/calibration.hpp
//
// Per-ticker constants calibrated from real NASDAQ LOBSTER order-book data
// (new-feature/DATASET_ANALYSIS.md, 8-phase analysis; derivation of every
// derived value below documented in new-feature/final_plan.md). Every raw
// figure here was extracted programmatically by
// new-feature/analysis/export_calibration.py, not hand-typed from the
// 4-decimal text in output_*.txt.
//
// ============================================================================
// UNITS WARNING -- read this before touching gamma_default or sigma anywhere
// in this file. This is not a stylistic note: getting it backwards produces
// a strategy that compiles, runs, and silently quotes at wildly wrong prices.
// ============================================================================
//   - hydra::Order::price is int64_t CENTS. Every price-like quantity in
//     this file -- sigma, spreads, the reservation-price skew gamma
//     produces -- is in CENTS, never dollars.
//   - The Python analysis reports sigma in DOLLARS/sqrt(second). Every
//     sigma_cents_buckets value below is that figure * 100, already
//     converted -- do not re-derive sigma from dollars anywhere else.
//   - gamma_default MUST be (and was) derived using sigma in CENTS, not
//     dollars. Using sigma_dollars in that derivation understates gamma by
//     exactly 100^2 = 10,000x once plugged into the cents-based
//     reservation-price formula r = s - q*gamma*sigma^2*(T-t) -- this is
//     not hypothetical, it was caught happening during this file's own
//     construction (see final_plan.md's "second unit trap" note). If you
//     ever re-derive gamma_default, re-derive it in cents, and sanity-check
//     against phase-11 test #10 (skew at q_max, full T-t stays within a
//     small multiple of that ticker's own mean spread).
//
// ============================================================================
// RECALIBRATION NOTE (post-first-run) -- new-feature/analysis/derive_gamma_skew.py
// ============================================================================
//   The first backtest run (new-feature/RESULTS.md) found AAPL/AMZN pinned
//   at q_max, with signal skew outweighing inventory-control skew by
//   15-380x under typical (not corner) conditions. Root-caused to two
//   separate issues, both fixed by regenerating every sigma_cents_buckets
//   value and every gamma_default value below via derive_gamma_skew.py:
//     1. sigma_cents_buckets was computed from RAW tick-to-tick mid diffs
//        (export_calibration.py's sigma_buckets()), not resampled onto the
//        same fixed 1-second grid the session-level sigma (and everything
//        gamma_default was originally calibrated against) uses. Real
//        order-book updates arrive far more than once a second, so the raw
//        version was systematically too small (e.g. AAPL's open-of-session
//        bucket: 2.15c raw vs. 5.69c on a proper 1s grid) -- this silently
//        shrank live inventory-control AND (for STANDARD_K names) the
//        k-based spread itself, both of which consume sigma^2.
//     2. gamma_default's design point was (q_max, T_max) -- a 3x
//        safety-padded cap AND the maximum possible time-to-close,
//        simultaneously -- while skew_strength_default's design point was
//        a genuinely typical one (1 signal std). Re-targeted gamma_default
//        at q_typical = q_max / 3 (the expected size of ONE ordinary
//        directional run -- already implicit in how q_max itself is built,
//        not a new free parameter) and T_typical = T_max / 2. q_max's own
//        value and its role as the hard suppression cap are unchanged --
//        only the calibration target point moved.
//   Do not hand-edit sigma_cents_buckets or gamma_default independently of
//   each other or of derive_gamma_skew.py -- re-run that script and paste
//   its full output if either ever needs to change again.

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

namespace asmm {

// Session boundaries, matching analyze_dataset.py's SESSION_START_S/END_S
// exactly -- used as T (the A-S horizon) throughout this backtest.
inline constexpr double kSessionStartS = 34200.0;  // 09:30:00
inline constexpr double kSessionEndS = 57600.0;    // 16:00:00
inline constexpr int kSigmaBuckets = 13;           // 30-minute buckets
inline constexpr double kBucketSeconds =
    (kSessionEndS - kSessionStartS) / static_cast<double>(kSigmaBuckets);  // 1800.0

enum class KTreatment {
    kStandardK,     // AAPL, AMZN, GOOG -- k_bid/k_ask estimable, use the real A-S spread formula
    kNoKFallback,   // INTC, MSFT -- no viable k; quote at the 1-cent minimum tick instead
};

struct TickerCalibration {
    std::string_view symbol;
    KTreatment treatment;

    // sigma, CENTS per sqrt(second), one value per half-hour bucket,
    // 09:30-16:00. Index via sigma_for_time() below, not direct indexing.
    std::array<double, kSigmaBuckets> sigma_cents_buckets;

    // Lit-only fill-intensity decay, /cent. NaN when treatment ==
    // kNoKFallback -- never read in that path (STANDARD_K formula only).
    double k_bid_per_cent;
    double k_ask_per_cent;

    double mean_spread_cents;   // time-weighted real mean spread
    std::uint32_t quote_size;   // shares -- median lit-fill size for this ticker
    std::int64_t q_max;         // shares -- data-derived inventory cap

    // Derived (not hardcoded) per the units warning above -- see
    // final_plan.md's Calibration section for the exact formulas.
    double gamma_default;
    double skew_strength_default;

    // Fixed OLS composite weights (OFI, TradeFlowMom, MicroDev, intercept),
    // fit on z-scored inputs against 5s forward return, full session.
    // Only meaningful (and only applied) when treatment == kNoKFallback.
    double composite_weight_ofi;
    double composite_weight_momentum;
    double composite_weight_microdev;
    double composite_weight_intercept;
};

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

inline constexpr std::array<TickerCalibration, 5> kCalibrations{{
    // symbol, treatment, sigma_cents_buckets{13}, k_bid, k_ask,
    // mean_spread_cents, quote_size, q_max, gamma_default,
    // skew_strength_default, composite weights (ofi, mom, microdev, intercept)
    TickerCalibration{
        "AAPL", KTreatment::kStandardK,
        {5.691237, 4.734152, 3.138292, 3.627510, 3.068309, 3.114061, 2.611821,
         2.570696, 3.158226, 2.726590, 2.735076, 3.289276, 3.970620},
        0.407434, 0.290000,
        15.134439, 75, 2634,
        5.916598e-08, 1.643537e-02,
        kNaN, kNaN, kNaN, kNaN,
    },
    TickerCalibration{
        "AMZN", KTreatment::kStandardK,
        {3.436073, 2.538573, 2.052755, 1.899635, 1.701981, 1.860025, 1.637685,
         1.306102, 1.559801, 1.563136, 1.303127, 1.954736, 1.960000},
        0.373193, 0.293119,
        12.848502, 60, 2291,
        1.831016e-07, 1.263666e-02,
        kNaN, kNaN, kNaN, kNaN,
    },
    TickerCalibration{
        "GOOG", KTreatment::kStandardK,
        {8.907905, 5.108193, 3.866625, 3.129760, 3.155767, 2.810695, 2.982502,
         2.493176, 2.948061, 2.467983, 3.093481, 2.891861, 3.848945},
        0.066122, 0.052752,
        27.330023, 64, 1605,
        1.347279e-07, 6.983347e-02,
        kNaN, kNaN, kNaN, kNaN,
    },
    TickerCalibration{
        "INTC", KTreatment::kNoKFallback,
        {0.483098, 0.253382, 0.161619, 0.205266, 0.179552, 0.231389, 0.177187,
         0.146281, 0.172392, 0.101406, 0.162491, 0.206778, 0.235177},
        kNaN, kNaN,
        1.012650, 100, 5670,
        4.447224e-07, 2.337094e+02,
        0.00181401, 0.00011671, 0.00097249, -0.00016861,
    },
    TickerCalibration{
        "MSFT", KTreatment::kNoKFallback,
        {0.443099, 0.357941, 0.211517, 0.251574, 0.221145, 0.248503, 0.221145,
         0.204854, 0.205538, 0.195748, 0.171638, 0.244941, 0.295397},
        kNaN, kNaN,
        1.010395, 100, 4508,
        4.191535e-07, 1.927665e+02,
        0.00222355, -0.00001717, 0.00122134, -0.00018132,
    },
}};

// Returns nullptr if symbol isn't in the calibrated panel.
[[nodiscard]] inline const TickerCalibration *find_calibration(std::string_view symbol) noexcept {
    for (const auto &cal : kCalibrations) {
        if (cal.symbol == symbol) return &cal;
    }
    return nullptr;
}

// Session-phase sigma lookup (phase 4/8: a single session-constant sigma
// has a real, quantified 22-39% forecast error -- this is the fix).
// Clamps to the first/last bucket outside [kSessionStartS, kSessionEndS)
// rather than reading out of bounds.
[[nodiscard]] inline double sigma_for_time(const TickerCalibration &cal, double sim_time_s) noexcept {
    double offset = sim_time_s - kSessionStartS;
    int idx = static_cast<int>(offset / kBucketSeconds);
    if (idx < 0) idx = 0;
    if (idx >= kSigmaBuckets) idx = kSigmaBuckets - 1;
    return cal.sigma_cents_buckets[static_cast<std::size_t>(idx)];
}

}  // namespace asmm
