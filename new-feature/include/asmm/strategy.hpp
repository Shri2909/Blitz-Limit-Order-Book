#pragma once

// new-feature/include/asmm/strategy.hpp
//
// The Avellaneda-Stoikov quoting agent and the naive fixed-spread control
// it is measured against. Both rest real GTC orders in the same
// hydra::OrderBook the real historical flow is replayed into, so the
// Matcher's own price-time priority decides whether real aggressor flow
// hits them.

#include "asmm/calibration.hpp"
#include "asmm/lobster_replay.hpp"
#include "asmm/markout_sink.hpp"
#include "asmm/signals.hpp"
#include "hydra/matcher.hpp"
#include "hydra/order_book.hpp"
#include "hydra/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace asmm {

struct StrategyParams {
    double gamma = 0.0;            // 0 => use calibration's per-ticker default
    double skew_strength = -1.0;   // <0 => use calibration's per-ticker default
    bool enable_skew = true;       // false => naive fixed-spread control
};

// Shared quoting/accounting machinery. The A-S agent and the naive control
// differ only in how they compute (reservation price, half-spreads), so
// that is the single virtual point rather than two near-duplicate classes.
class StrategyBase {
public:
    StrategyBase(const TickerCalibration &cal, hydra::OrderBook &book, hydra::Matcher &matcher,
                  MarkoutSink &sink, StrategyParams params)
        : cal_(cal), params_(params), book_(book), matcher_(matcher), sink_(sink) {
        if (params_.gamma <= 0.0) params_.gamma = cal_.gamma_default;
        if (params_.skew_strength < 0.0) params_.skew_strength = cal_.skew_strength_default;
    }
    virtual ~StrategyBase() = default;

    // Cancel both resting quotes, recompute, re-place. Always both sides,
    // never a price-diff optimisation -- see final_plan.md: with 86-91% of
    // real orders cancelled before filling, fast full requoting is what
    // this market actually does, and it avoids a class of stale-priority
    // bugs for a cost that is irrelevant in a non-latency backtest.
    void on_requote_tick(double sim_time_s, SignalState &signals) {
        cancel_resting();

        const L1State l1 = read_l1(book_);
        if (!l1.valid) return;  // one-sided book: nothing to quote around

        const double mid = 0.5 * (static_cast<double>(l1.bid_price) + static_cast<double>(l1.ask_price));
        const double t_minus_t = std::max(0.0, kSessionEndS - sim_time_s);
        const double sigma = sigma_for_time(cal_, sim_time_s);

        double reservation = mid;
        double half_bid = 0.0;
        double half_ask = 0.0;
        compute_quotes(mid, t_minus_t, sigma, sim_time_s, signals, l1, reservation, half_bid, half_ask);

        std::int64_t bid_px = static_cast<std::int64_t>(std::llround(reservation - half_bid));
        std::int64_t ask_px = static_cast<std::int64_t>(std::llround(reservation + half_ask));
        if (ask_px <= bid_px) ask_px = bid_px + 1;  // never quote crossed/locked

        // Stay passive: a bid at or above the best ask (or an ask at or
        // below the best bid) is an immediately-marketable order, not a
        // quote, and this strategy is a liquidity PROVIDER. Clamp only
        // against CROSSING -- deliberately NOT against improving. A quote
        // inside the current spread is exactly what a market maker running
        // a tighter spread than the market should do: it becomes the new
        // touch and earns queue priority. Clamping to the existing touch
        // instead would force the strategy to always join the back of the
        // real FIFO queue, which in a first run here produced 3 fills in
        // 300 seconds -- realistic for a queue-joiner, useless as a test of
        // the quoting logic.
        bid_px = std::min(bid_px, l1.ask_price - 1);
        ask_px = std::max(ask_px, l1.bid_price + 1);

        // q_max: suppress only the side that would grow |inventory| further,
        // keep quoting the reducing side.
        const bool want_bid = inventory_ < cal_.q_max;
        const bool want_ask = inventory_ > -cal_.q_max;

        if (want_bid) bid_id_ = place(hydra::Side::BUY, bid_px, cal_.quote_size, sim_time_s);
        if (want_ask) ask_id_ = place(hydra::Side::SELL, ask_px, cal_.quote_size, sim_time_s);
    }

    // Called by the engine for any FillEvent whose maker_order_id is one of
    // ours.
    void on_fill(const hydra::FillEvent &fill, double sim_time_s, double mid_cents) {
        hydra::Side side;
        if (bid_id_.has_value() && fill.maker_order_id == *bid_id_) {
            side = hydra::Side::BUY;
        } else if (ask_id_.has_value() && fill.maker_order_id == *ask_id_) {
            side = hydra::Side::SELL;
        } else {
            return;  // not one of our currently-tracked quotes
        }

        const std::int64_t qty = static_cast<std::int64_t>(fill.qty);
        if (side == hydra::Side::BUY) {
            inventory_ += qty;
            cash_cents_ -= fill.price * qty;
        } else {
            inventory_ -= qty;
            cash_cents_ += fill.price * qty;
        }
        ++fill_count_;

        sink_.record_fill(FillRecord{sim_time_s, side, fill.price, fill.qty,
                                      static_cast<std::int64_t>(std::llround(mid_cents)), inventory_,
                                      cash_cents_});
    }

    // Quantity of ours resting at a price -- lets resync reconcile market
    // liquidity without fighting the strategy's own quotes.
    [[nodiscard]] std::uint32_t qty_at_price(std::int64_t price) const {
        std::uint32_t total = 0;
        for (const auto &id : {bid_id_, ask_id_}) {
            if (!id.has_value()) continue;
            const hydra::Order *o = book_.find_order(*id);
            if (o != nullptr && o->price == price) total += o->qty;
        }
        return total;
    }

    [[nodiscard]] std::int64_t inventory() const noexcept { return inventory_; }
    [[nodiscard]] std::int64_t cash_cents() const noexcept { return cash_cents_; }
    [[nodiscard]] std::uint64_t fill_count() const noexcept { return fill_count_; }

    // Total P&L marks remaining inventory to market -- reporting cash alone
    // would over/understate depending on the sign of the position. This is
    // mark-to-market, explicitly NOT a forced liquidation (which would need
    // an execution-cost assumption nothing here calibrated).
    [[nodiscard]] double total_pnl_cents(double final_mid_cents) const noexcept {
        return static_cast<double>(cash_cents_) + static_cast<double>(inventory_) * final_mid_cents;
    }

protected:
    virtual void compute_quotes(double mid, double t_minus_t, double sigma_cents, double sim_time_s,
                                 SignalState &signals, const L1State &l1, double &reservation,
                                 double &half_bid, double &half_ask) = 0;

    const TickerCalibration &cal_;
    StrategyParams params_;

private:
    void cancel_resting() {
        if (bid_id_.has_value()) {
            (void)book_.cancel_order(*bid_id_);
            bid_id_.reset();
        }
        if (ask_id_.has_value()) {
            (void)book_.cancel_order(*ask_id_);
            ask_id_.reset();
        }
    }

    std::optional<std::uint64_t> place(hydra::Side side, std::int64_t price, std::uint32_t qty,
                                        double sim_time_s) {
        hydra::Order o{};
        o.order_id = next_order_id_++;
        // One shared client_id for all our quotes, so self-trade prevention
        // correctly stops our own bid and ask from crossing each other --
        // see kClientIdNote in lobster_replay.hpp.
        o.client_id = kStrategyClientId;
        o.price = price;
        o.qty = qty;
        o.side = side;
        o.tif = hydra::TimeInForce::GTC;
        o.timestamp_ns = static_cast<std::uint64_t>(sim_time_s * 1e9);

        // Passive quote: it should rest, not cross. If it would cross we
        // already clamped it above, so add_order directly rather than
        // routing through the Matcher.
        if (book_.add_order(o) == nullptr) return std::nullopt;
        return o.order_id;
    }

    hydra::OrderBook &book_;
    hydra::Matcher &matcher_;
    MarkoutSink &sink_;
    std::optional<std::uint64_t> bid_id_;
    std::optional<std::uint64_t> ask_id_;
    std::int64_t inventory_ = 0;
    std::int64_t cash_cents_ = 0;
    std::uint64_t fill_count_ = 0;
    std::uint64_t next_order_id_ = kStrategyOrderIdBase;
};

class AvellanedaStoikovStrategy : public StrategyBase {
public:
    using StrategyBase::StrategyBase;

protected:
    void compute_quotes(double mid, double t_minus_t, double sigma_cents, double sim_time_s,
                         SignalState &signals, const L1State &l1, double &reservation,
                         double &half_bid, double &half_ask) override {
        const double gamma = params_.gamma;
        const double sigma2 = sigma_cents * sigma_cents;

        // r(s,q,t) = s - q*gamma*sigma^2*(T-t), sigma in CENTS so the whole
        // expression lands in cents (see calibration.hpp's units warning).
        reservation = mid - static_cast<double>(inventory()) * gamma * sigma2 * t_minus_t;

        if (params_.enable_skew) {
            reservation += params_.skew_strength * signals.skew_signal(cal_, sim_time_s, l1);
        }

        if (cal_.treatment == KTreatment::kStandardK) {
            // Per-side asymmetric spread using the separately calibrated
            // k_bid/k_ask -- phase 2 found this asymmetry real and
            // consistent in every name where k is estimable.
            const double inv_term = gamma * sigma2 * t_minus_t / 2.0;
            half_bid = inv_term + (1.0 / gamma) * std::log1p(gamma / cal_.k_bid_per_cent);
            half_ask = inv_term + (1.0 / gamma) * std::log1p(gamma / cal_.k_ask_per_cent);
        } else {
            // NO_K_FALLBACK (INTC/MSFT): no k was ever estimable -- their
            // spread is pinned at the 1c minimum tick 98.8-99.0% of the
            // session. Quote the minimum tick (half a cent each side of the
            // skewed reservation price, which rounds to adjacent integer
            // cents) and let the validated composite signal do the
            // directional work the k-formula would otherwise do.
            half_bid = 0.5;
            half_ask = 0.5;
        }
    }
};

class NaiveFixedSpreadStrategy : public StrategyBase {
public:
    using StrategyBase::StrategyBase;

protected:
    void compute_quotes(double mid, double /*t_minus_t*/, double /*sigma_cents*/,
                         double /*sim_time_s*/, SignalState & /*signals*/, const L1State & /*l1*/,
                         double &reservation, double &half_bid, double &half_ask) override {
        // The control: quote symmetrically around the raw mid at the
        // ticker's own measured mean spread. No inventory skew, no signal,
        // no treatment branching -- one code path for all five names. This
        // is the "what naive quoting actually does" baseline whose real-data
        // economics phase 8 already characterised.
        reservation = mid;
        half_bid = cal_.mean_spread_cents / 2.0;
        half_ask = cal_.mean_spread_cents / 2.0;
    }
};

}  // namespace asmm
