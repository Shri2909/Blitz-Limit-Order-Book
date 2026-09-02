#pragma once

// new-feature/include/asmm/markout_sink.hpp
//
// Records every fill the strategy receives, plus a trace of the real mid
// over the session, and computes post-run markout at fixed horizons.
//
// Deliberately a plain std::vector, not the atomic-write-index
// RawSampleSink pattern from include/hydra/pipeline.hpp: that struct's
// atomics exist because a producer thread and a consumer thread touch it
// concurrently. This backtest is single-threaded end to end, so the
// simpler container is the correct analog, not an oversight.
//
// Statistical treatment of these rows (HAC/Newey-West significance) stays
// a Python job on the emitted CSV, reusing
// analyze_dataset_riskparams.py's newey_west_se -- consistent with this
// project's existing C++-does-mechanics / Python-does-statistics split.

#include "hydra/types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace asmm {

struct FillRecord {
    double time_s;
    hydra::Side maker_side;   // the side the STRATEGY was resting on
    std::int64_t price_cents;
    std::uint32_t qty;
    std::int64_t mid_at_fill_cents;
    std::int64_t inventory_after;
    std::int64_t cash_after_cents;
};

struct MidSample {
    double time_s;
    double mid_cents;
};

class MarkoutSink {
public:
    void record_fill(const FillRecord &rec) { fills_.push_back(rec); }

    void record_mid(double time_s, double mid_cents) {
        // One sample per distinct timestamp is plenty for markout lookup,
        // and keeps the trace from ballooning on burst timestamps.
        if (!mids_.empty() && mids_.back().time_s == time_s) {
            mids_.back().mid_cents = mid_cents;
            return;
        }
        mids_.push_back({time_s, mid_cents});
    }

    [[nodiscard]] const std::vector<FillRecord> &fills() const noexcept { return fills_; }
    [[nodiscard]] bool empty() const noexcept { return fills_.empty(); }

    // Mid at or before `t` (as-of lookup), matching how the Python markout
    // analysis indexed forward prices.
    [[nodiscard]] double mid_as_of(double t) const noexcept {
        if (mids_.empty()) return 0.0;
        auto it = std::upper_bound(mids_.begin(), mids_.end(), t,
                                    [](double v, const MidSample &s) { return v < s.time_s; });
        if (it == mids_.begin()) return mids_.front().mid_cents;
        return (it - 1)->mid_cents;
    }

    [[nodiscard]] double final_mid_cents() const noexcept {
        return mids_.empty() ? 0.0 : mids_.back().mid_cents;
    }

    // Provider-side markout: how far the mid moved in the direction of the
    // position this fill created, `horizon` seconds later. Negative means
    // adverse selection -- the same sign convention the Python analysis
    // used (provider_sign * (future_mid - mid_at_fill)).
    [[nodiscard]] double markout_cents(const FillRecord &rec, double horizon_s) const noexcept {
        const double future_mid = mid_as_of(rec.time_s + horizon_s);
        const double provider_sign = (rec.maker_side == hydra::Side::BUY) ? 1.0 : -1.0;
        return provider_sign * (future_mid - static_cast<double>(rec.mid_at_fill_cents));
    }

    bool write_csv(const std::string &path) const {
        std::FILE *f = std::fopen(path.c_str(), "w");
        if (f == nullptr) return false;
        std::fprintf(f,
                     "time_s,maker_side,price_cents,qty,mid_at_fill_cents,inventory_after,"
                     "cash_after_cents,markout_1s_cents,markout_5s_cents,markout_30s_cents\n");
        for (const auto &r : fills_) {
            std::fprintf(f, "%.9f,%s,%lld,%u,%lld,%lld,%lld,%.6f,%.6f,%.6f\n", r.time_s,
                          (r.maker_side == hydra::Side::BUY) ? "BUY" : "SELL",
                          static_cast<long long>(r.price_cents), r.qty,
                          static_cast<long long>(r.mid_at_fill_cents),
                          static_cast<long long>(r.inventory_after),
                          static_cast<long long>(r.cash_after_cents), markout_cents(r, 1.0),
                          markout_cents(r, 5.0), markout_cents(r, 30.0));
        }
        std::fclose(f);
        return true;
    }

    [[nodiscard]] double mean_markout_cents(double horizon_s) const noexcept {
        if (fills_.empty()) return 0.0;
        double total = 0.0;
        for (const auto &r : fills_) total += markout_cents(r, horizon_s);
        return total / static_cast<double>(fills_.size());
    }

private:
    std::vector<FillRecord> fills_;
    std::vector<MidSample> mids_;
};

}  // namespace asmm
