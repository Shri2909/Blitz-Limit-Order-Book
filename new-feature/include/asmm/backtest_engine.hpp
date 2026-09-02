#pragma once

// new-feature/include/asmm/backtest_engine.hpp
//
// The unified event loop: replay + signals + strategy + markout sink.
// Per replayed message row, in order (ordering matters -- see below):
//   1. apply the LOBSTER event to the book/matcher
//   2. resync market liquidity to the real snapshot for this row
//   3. feed the resulting L1 transition to OFI, but ONLY for changes
//      caused by real market activity, never the strategy's own requoting
//   4. route any fill against one of our quotes to the strategy + sink
//   5. advance sim time; requote if the interval has elapsed
//   6. append the real mid to the markout price trace

#include "asmm/calibration.hpp"
#include "asmm/lobster_replay.hpp"
#include "asmm/markout_sink.hpp"
#include "asmm/signals.hpp"
#include "asmm/strategy.hpp"
#include "hydra/matcher.hpp"
#include "hydra/object_pool.hpp"
#include "hydra/order_book.hpp"

#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace asmm {

struct BacktestConfig {
    std::string message_csv;
    std::string orderbook_csv;
    int n_levels = 10;
    double requote_interval_s = 0.25;  // see final_plan.md for why 250ms
    double duration_seconds = 0.0;     // 0 => full session
};

struct BacktestResult {
    std::uint64_t rows_replayed = 0;
    std::uint64_t fill_count = 0;
    std::int64_t final_inventory = 0;
    std::int64_t final_cash_cents = 0;
    double final_mid_cents = 0.0;
    double total_pnl_cents = 0.0;
    double mean_markout_1s = 0.0;
    double mean_markout_5s = 0.0;
    double mean_markout_30s = 0.0;
};

// StrategyFactory: (cal, book, matcher, sink, params) -> unique_ptr<StrategyBase>
template <typename StrategyFactory>
BacktestResult run_backtest(const TickerCalibration &cal, const BacktestConfig &cfg,
                             StrategyParams params, StrategyFactory &&make_strategy,
                             MarkoutSink &sink) {
    auto order_pool = std::make_unique<hydra::ObjectPool<hydra::Order, hydra::ORDER_POOL_SIZE>>();
    auto level_pool = std::make_unique<hydra::ObjectPool<hydra::Level, hydra::LEVEL_POOL_SIZE>>();
    hydra::OrderBook book(*order_pool, *level_pool);
    hydra::Matcher matcher(book, hydra::MatchingMode::PRICE_TIME);

    LobsterReplayEngine replay;
    SignalState signals;
    auto strategy = make_strategy(cal, book, matcher, sink, params);

    LobsterMessageStream messages(cfg.message_csv);
    std::ifstream orderbook(cfg.orderbook_csv);
    if (!orderbook) throw std::runtime_error("cannot open orderbook file: " + cfg.orderbook_csv);

    std::string ob_line;
    if (!std::getline(orderbook, ob_line)) throw std::runtime_error("orderbook file is empty");
    replay.seed_from_orderbook_row0(book, ob_line, cfg.n_levels);
    (void)messages.next();  // row 0's effect is already inside the seed

    BacktestResult result{};
    double last_requote_s = 0.0;
    bool first_tick = true;
    const double stop_at_s =
        (cfg.duration_seconds > 0.0) ? (kSessionStartS + cfg.duration_seconds) : kSessionEndS;

    L1State prev_l1 = read_l1(book);

    while (auto ev = messages.next()) {
        if (!std::getline(orderbook, ob_line)) break;
        if (ev->time_s > stop_at_s) break;

        const double now = ev->time_s;

        // (1) apply the real event. Fills against our own quotes are routed
        // straight through to the strategy; fills against anything else are
        // real-market-vs-real-market and only matter for the trade tape.
        auto on_fill = [&](const hydra::FillEvent &fill) {
            if (is_strategy_order_id(fill.maker_order_id)) {
                const L1State l1_now = read_l1(book);
                const double mid_now =
                    l1_now.valid ? 0.5 * (static_cast<double>(l1_now.bid_price) +
                                           static_cast<double>(l1_now.ask_price))
                                 : 0.0;
                strategy->on_fill(fill, now, mid_now);
            }
        };
        replay.apply_event(*ev, book, matcher, on_fill);

        // Lit executions drive the trade-flow-momentum tape regardless of
        // who was on the other side.
        if (ev->event_type == 4) signals.on_lit_fill(now, ev->direction, ev->size);

        // (2) reconcile market liquidity to the real snapshot, excluding
        // whatever of the book's quantity is the strategy's own.
        replay.resync_to_snapshot(book, ob_line, cfg.n_levels,
                                   [&](std::int64_t price) { return strategy->qty_at_price(price); });

        // (3) OFI sees the transition produced by real market activity.
        // The strategy's own requote below is deliberately excluded: it
        // happens after this point, and its effect is folded into the NEXT
        // row's prev_l1 baseline only via market changes, never as an OFI
        // increment of its own.
        const L1State curr_l1 = read_l1(book);
        signals.on_market_book_change(now, prev_l1, curr_l1);

        // (6) price trace for post-run markout.
        if (curr_l1.valid) {
            const double mid = 0.5 * (static_cast<double>(curr_l1.bid_price) +
                                       static_cast<double>(curr_l1.ask_price));
            sink.record_mid(now, mid);
            result.final_mid_cents = mid;
        }

        // (5) requote on schedule.
        if (first_tick || (now - last_requote_s) >= cfg.requote_interval_s) {
            strategy->on_requote_tick(now, signals);
            last_requote_s = now;
            first_tick = false;
        }

        // Re-read AFTER our own requoting so the next row's OFI baseline
        // already contains our quotes -- otherwise our own placement would
        // register as a market-driven L1 change on the next transition.
        prev_l1 = read_l1(book);
        ++result.rows_replayed;
    }

    result.fill_count = strategy->fill_count();
    result.final_inventory = strategy->inventory();
    result.final_cash_cents = strategy->cash_cents();
    result.total_pnl_cents = strategy->total_pnl_cents(result.final_mid_cents);
    result.mean_markout_1s = sink.mean_markout_cents(1.0);
    result.mean_markout_5s = sink.mean_markout_cents(5.0);
    result.mean_markout_30s = sink.mean_markout_cents(30.0);
    return result;
}

}  // namespace asmm
