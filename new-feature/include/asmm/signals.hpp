#pragma once

// new-feature/include/asmm/signals.hpp
//
// The three live signals the strategy reads, computed from the REAL
// replayed order flow. Every formula here is a direct port of the
// already-validated Python reference in new-feature/analysis/ -- the
// numbers those scripts produced (IC, decay, out-of-sample validation) only
// apply to the strategy if the C++ computes the same quantity, so the
// conventions below are deliberately literal, not "equivalent".
//
//   OFI                  <- analyze_dataset_alpha.py::compute_ofi_events
//   micro-price deviation<- analyze_dataset_alpha2.py (micro - mid, cents)
//   trade-flow momentum  <- analyze_dataset_alpha2.py (lit fills only)

#include "asmm/calibration.hpp"
#include "hydra/order_book.hpp"
#include "hydra/types.hpp"

#include <cmath>
#include <cstdint>
#include <deque>

namespace asmm {

// Welford online mean/variance, used to z-score composite components the
// same way analyze_dataset_alpha3.py's zscore() did before the OLS fit --
// the stored weights are coefficients on STANDARDIZED inputs, so feeding
// them raw values would be meaningless.
class RunningMoments {
public:
    void add(double x) noexcept {
        ++n_;
        const double delta = x - mean_;
        mean_ += delta / static_cast<double>(n_);
        m2_ += delta * (x - mean_);
    }
    [[nodiscard]] double mean() const noexcept { return mean_; }
    [[nodiscard]] double stddev() const noexcept {
        if (n_ < 2) return 0.0;
        return std::sqrt(m2_ / static_cast<double>(n_ - 1));
    }
    [[nodiscard]] double zscore(double x) const noexcept {
        const double s = stddev();
        return (s > 0.0) ? (x - mean_) / s : 0.0;
    }

private:
    std::uint64_t n_ = 0;
    double mean_ = 0.0;
    double m2_ = 0.0;
};

// Trailing-window sum over timestamped increments.
//
// The window length is NOT a free choice: OFI's validated IC was measured
// on a 1-SECOND bucketed grid, so "1 second of accumulated flow" is the
// unit those correlation numbers describe. Reading OFI as "whatever
// accumulated since the last requote tick" (250ms by default) would be a
// different, unvalidated quantity -- see final_plan.md. Same reasoning
// pins trade-flow momentum to its own validated 5-second window.
class TrailingWindowSum {
public:
    explicit TrailingWindowSum(double window_seconds) noexcept : window_s_(window_seconds) {}

    void add(double time_s, double value) noexcept {
        entries_.push_back({time_s, value});
        sum_ += value;
        evict(time_s);
    }

    [[nodiscard]] double sum_as_of(double time_s) noexcept {
        evict(time_s);
        return sum_;
    }

private:
    void evict(double now_s) noexcept {
        const double cutoff = now_s - window_s_;
        while (!entries_.empty() && entries_.front().time_s < cutoff) {
            sum_ -= entries_.front().value;
            entries_.pop_front();
        }
    }

    struct Entry {
        double time_s;
        double value;
    };
    std::deque<Entry> entries_;
    double window_s_;
    double sum_ = 0.0;
};

inline constexpr double kOfiWindowSeconds = 1.0;       // matches phase 5's 1s grid
inline constexpr double kMomentumWindowSeconds = 5.0;  // matches MOMENTUM_WINDOW_S

// Snapshot of L1 used for the OFI transition computation.
struct L1State {
    std::int64_t bid_price = 0;
    std::uint32_t bid_size = 0;
    std::int64_t ask_price = 0;
    std::uint32_t ask_size = 0;
    bool valid = false;
};

[[nodiscard]] inline L1State read_l1(hydra::OrderBook &book) noexcept {
    L1State s{};
    hydra::Level *bid = book.best_bid_level();
    hydra::Level *ask = book.best_ask_level();
    if (bid != nullptr) {
        s.bid_price = bid->price;
        s.bid_size = bid->total_qty;
    }
    if (ask != nullptr) {
        s.ask_price = ask->price;
        s.ask_size = ask->total_qty;
    }
    s.valid = (bid != nullptr && ask != nullptr);
    return s;
}

// Cont/Kukanov/Stoikov order-flow-imbalance increment for one L1 transition.
// Ported branch-for-branch from compute_ofi_events():
//   e_bid = q_bid            if P_bid >  P_bid_prev
//         = -q_bid_prev      if P_bid <  P_bid_prev
//         = q_bid - q_bid_prev  otherwise
//   e_ask = q_ask            if P_ask <  P_ask_prev
//         = -q_ask_prev      if P_ask >  P_ask_prev
//         = q_ask - q_ask_prev  otherwise
//   OFI   = e_bid - e_ask
[[nodiscard]] inline double ofi_increment(const L1State &prev, const L1State &curr) noexcept {
    const double e_bid =
        (curr.bid_price > prev.bid_price)   ? static_cast<double>(curr.bid_size)
        : (curr.bid_price < prev.bid_price) ? -static_cast<double>(prev.bid_size)
                                            : static_cast<double>(curr.bid_size) -
                                                  static_cast<double>(prev.bid_size);
    const double e_ask =
        (curr.ask_price < prev.ask_price)   ? static_cast<double>(curr.ask_size)
        : (curr.ask_price > prev.ask_price) ? -static_cast<double>(prev.ask_size)
                                            : static_cast<double>(curr.ask_size) -
                                                  static_cast<double>(prev.ask_size);
    return e_bid - e_ask;
}

// Size-weighted mid minus plain mid, in cents (analyze_dataset_alpha2.py):
//   micro = (bid_sz*ask_px + ask_sz*bid_px) / (bid_sz + ask_sz)
[[nodiscard]] inline double micro_price_deviation_cents(const L1State &s) noexcept {
    const double denom = static_cast<double>(s.bid_size) + static_cast<double>(s.ask_size);
    if (!s.valid || denom <= 0.0) return 0.0;
    const double micro = (static_cast<double>(s.bid_size) * static_cast<double>(s.ask_price) +
                          static_cast<double>(s.ask_size) * static_cast<double>(s.bid_price)) /
                         denom;
    const double mid = 0.5 * (static_cast<double>(s.bid_price) + static_cast<double>(s.ask_price));
    return micro - mid;
}

class SignalState {
public:
    SignalState() : ofi_(kOfiWindowSeconds), momentum_(kMomentumWindowSeconds) {}

    // Feed one L1 transition caused by REAL replayed market activity.
    // The caller is responsible for NOT calling this for the strategy's own
    // quote placement/cancellation -- otherwise the "signal" would react to
    // the strategy's own repricing rather than to market flow (see
    // final_plan.md's exclusion rule).
    void on_market_book_change(double time_s, const L1State &prev, const L1State &curr) noexcept {
        if (!prev.valid || !curr.valid) return;
        ofi_.add(time_s, ofi_increment(prev, curr));
    }

    // Feed one LIT (event_type 4) execution. Sign convention is the Python
    // reference's exactly: signed_vol = -direction * size, which is
    // inverted from the intuitive reading and pairs with the stored OLS
    // weight -- flipping it silently inverts the composite.
    void on_lit_fill(double time_s, int direction, std::uint32_t size) noexcept {
        momentum_.add(time_s, -static_cast<double>(direction) * static_cast<double>(size));
    }

    [[nodiscard]] double ofi(double time_s) noexcept { return ofi_.sum_as_of(time_s); }
    [[nodiscard]] double momentum(double time_s) noexcept { return momentum_.sum_as_of(time_s); }

    // The value the strategy actually skews on.
    //   STANDARD_K  -> OFI alone (phase 7: combining did not help these names)
    //   NO_K_FALLBACK -> the OLS composite validated out-of-sample for
    //                    INTC/MSFT, on z-scored components.
    [[nodiscard]] double skew_signal(const TickerCalibration &cal, double time_s,
                                      const L1State &l1) noexcept {
        const double raw_ofi = ofi(time_s);
        if (cal.treatment == KTreatment::kStandardK) {
            return raw_ofi;
        }
        const double raw_mom = momentum(time_s);
        const double raw_micro = micro_price_deviation_cents(l1);

        ofi_stats_.add(raw_ofi);
        mom_stats_.add(raw_mom);
        micro_stats_.add(raw_micro);

        return cal.composite_weight_ofi * ofi_stats_.zscore(raw_ofi) +
               cal.composite_weight_momentum * mom_stats_.zscore(raw_mom) +
               cal.composite_weight_microdev * micro_stats_.zscore(raw_micro) +
               cal.composite_weight_intercept;
    }

private:
    TrailingWindowSum ofi_;
    TrailingWindowSum momentum_;
    RunningMoments ofi_stats_;
    RunningMoments mom_stats_;
    RunningMoments micro_stats_;
};

}  // namespace asmm
