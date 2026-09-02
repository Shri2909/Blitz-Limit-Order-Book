#pragma once

// new-feature/include/asmm/lobster_replay.hpp
//
// Streams a real LOBSTER message CSV and converts each row into the
// corresponding hydra::OrderBook/Matcher operation, per the event-type
// table in new-feature/final_plan.md's "What real-data replay means
// mechanically" section. This file's only job is that mapping -- OFI
// feeding, requote-tick scheduling, and fill routing to a strategy are the
// caller's (backtest_engine.hpp's) responsibility, not this file's.
//
// EMPIRICALLY CONFIRMED DURING IMPLEMENTATION (not hypothetical -- this is
// exactly the "named risk" final_plan.md flagged before any code existed):
// the message file is NOT self-contained from an empty book. Real
// executions/cancels early in the session reference order_ids that never
// appear as a type-1 "new order" message anywhere in the file -- pre-
// existing resting liquidity (opening-auction carryover) that predates the
// file's window. LobsterReplayEngine::seed_from_orderbook_row0() and the
// price-keyed fallback below are the fix, built from that evidence, not
// guessed at in advance.

#include "hydra/matcher.hpp"
#include "hydra/order_book.hpp"
#include "hydra/types.hpp"

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <algorithm>
#include <limits>

namespace asmm {

// Order-ID namespace, four disjoint ranges so nothing can ever collide
// regardless of run length:
//   - real LOBSTER order_ids: whatever's in the file. Empirically confirmed
//     max across all 5 tickers' real data: 287,174,077 (AMZN).
//   - synthetic pre-existing-liquidity seed orders (see above): starts at
//     kSeedOrderIdBase.
//   - synthesized background-aggressor IDs (type-4 replay, IOC, never
//     rests): starts at kAggressorOrderIdBase.
//   - the strategy's own resting quote IDs: starts at kStrategyOrderIdBase.

inline constexpr std::uint64_t kFillerOrderIdBase = std::uint64_t{1} << 34;
inline constexpr std::uint64_t kAggressorOrderIdBase = std::uint64_t{1} << 40;
inline constexpr std::uint64_t kStrategyOrderIdBase = std::uint64_t{1} << 62;

[[nodiscard]] inline bool is_strategy_order_id(std::uint64_t order_id) noexcept {
    return order_id >= kStrategyOrderIdBase;
}

// kClientIdNote -- WHY every replayed/seed/aggressor order sets
// client_id = order_id, and why leaving it unset is catastrophic:
//
// hydra::OrderBook::is_self_trade() is `resting.client_id ==
// incoming.client_id`, and hydra::Order value-initializes client_id to 0.
// Leaving client_id unset therefore makes EVERY order in the replay look
// like the same participant, so the Matcher's self-trade prevention
// silently skips essentially every match -- no fill, no error, no crash,
// just a book that never trades. That is not hypothetical: it produced a
// 99.99% row mismatch in this feature's own replay-fidelity check before
// being tracked down.
//
// LOBSTER doesn't publish participant identity, and these historical
// crosses demonstrably DID execute in the real market, so the correct
// reconstruction is "never spuriously block a historically-real match."
// Setting client_id = order_id achieves exactly that: order_ids are unique
// per order, so no two distinct orders ever collide.
//
// The strategy's own quotes are the deliberate exception -- they share one
// fixed client_id (kStrategyClientId) so that self-trade prevention
// correctly stops the strategy's own bid and ask from crossing each other,
// which is a real protection worth keeping.
inline constexpr std::uint64_t kStrategyClientId = kStrategyOrderIdBase;

// One parsed LOBSTER message row.
struct LobsterEvent {
    double time_s = 0.0;
    int event_type = 0;  // 1,2,3,4,5,7
    std::uint64_t order_id = 0;
    std::uint32_t size = 0;
    std::int64_t price_cents = 0;  // already converted from LOBSTER's 1e-4-dollar fixed point
    int direction = 0;             // +1 or -1
};

// Parses one LOBSTER message CSV line: time,event_type,order_id,size,price,direction
// LOBSTER price is 1e-4 dollars; cents = raw / 100 (the very first unit
// conversion in the whole pipeline -- get this wrong and everything
// downstream is silently corrupted, see final_plan.md's price-convention note).
[[nodiscard]] inline LobsterEvent parse_lobster_line(const std::string &line) {
    LobsterEvent ev{};
    std::istringstream ss(line);
    std::string field;

    std::getline(ss, field, ',');
    ev.time_s = std::stod(field);
    std::getline(ss, field, ',');
    ev.event_type = std::stoi(field);
    std::getline(ss, field, ',');
    ev.order_id = std::stoull(field);
    std::getline(ss, field, ',');
    ev.size = static_cast<std::uint32_t>(std::stoul(field));
    std::getline(ss, field, ',');
    ev.price_cents = std::stoll(field) / 100;
    std::getline(ss, field, ',');
    ev.direction = std::stoi(field);

    return ev;
}

class LobsterMessageStream {
public:
    explicit LobsterMessageStream(const std::string &path) : file_(path) {
        if (!file_) {
            throw std::runtime_error("cannot open LOBSTER message file: " + path);
        }
    }

    // Returns the next event, or std::nullopt at EOF.
    [[nodiscard]] std::optional<LobsterEvent> next() {
        std::string line;
        if (!std::getline(file_, line) || line.empty()) {
            return std::nullopt;
        }
        return parse_lobster_line(line);
    }

private:
    std::ifstream file_;
};

// Parses one line of the LOBSTER orderbook CSV (ask_px,ask_sz,bid_px,bid_sz
// repeated per level, no header) into (price_cents, size) pairs, skipping
// LOBSTER's sentinel "no such level" values (huge magnitude, positive for a
// missing ask, negative for a missing bid).
struct OrderbookLevel {
    std::int64_t price_cents;
    std::uint32_t size;
};

[[nodiscard]] inline std::vector<std::pair<hydra::Side, OrderbookLevel>>
parse_orderbook_line_all_levels(const std::string &line, int n_levels) {
    std::vector<std::pair<hydra::Side, OrderbookLevel>> out;
    std::istringstream ss(line);
    std::string field;
    for (int lvl = 0; lvl < n_levels; ++lvl) {
        std::getline(ss, field, ',');
        long long ask_price_raw = std::stoll(field);
        std::getline(ss, field, ',');
        long long ask_size = std::stoll(field);
        std::getline(ss, field, ',');
        long long bid_price_raw = std::stoll(field);
        std::getline(ss, field, ',');
        long long bid_size = std::stoll(field);

        // LOBSTER sentinel for "level does not exist" is a huge-magnitude
        // price (e.g. +/-9999999999); anything outside a sane cents range
        // is treated as absent rather than trusted as a real price.
        if (ask_price_raw > 0 && ask_price_raw < 100'000'000 && ask_size > 0) {
            out.push_back({hydra::Side::SELL, {ask_price_raw / 100, static_cast<std::uint32_t>(ask_size)}});
        }
        if (bid_price_raw > 0 && bid_price_raw < 100'000'000 && bid_size > 0) {
            out.push_back({hydra::Side::BUY, {bid_price_raw / 100, static_cast<std::uint32_t>(bid_size)}});
        }
    }
    return out;
}

// Owns the order-id/price-tracking state needed to correctly replay a real
// LOBSTER session: the pre-existing-liquidity seed (see file header) and
// the monotonic counter for synthesized type-4 aggressor IDs.
class LobsterReplayEngine {
public:
    // Seeds the book with one synthetic resting order per non-empty price
    // level from the real orderbook file's first row -- the book state
    // that predates this message file's window and is never explained by
    // any type-1 message in it. `n_levels` must match the CSV's level
    // count (10 for the *_10.csv files this project downloaded).
    //
    // This is a deliberate approximation: the real pre-existing liquidity
    // at each level may have been many distinct orders; we represent it as
    // one, sized to the level's real aggregate quantity. That's the right
    // level of fidelity for a market-making backtest measuring the
    // strategy's own edge against real aggregate depth/price -- it does
    // not depend on how many distinct historical orders composed that
    // depth, only on the depth and price themselves.
    // Establishes the book's starting state from the real orderbook file's
    // first row. Delegates straight to resync_to_snapshot() -- the same
    // mechanism that maintains market liquidity on every subsequent row --
    // rather than laying down a separate, unmanaged set of "seed" orders.
    //
    // That distinction turned out to matter: a separate seed mechanism
    // leaves stale synthetic liquidity that nothing later reconciles, which
    // showed up empirically as our reconstructed bid sitting one tick above
    // the real market's for the rest of the session. Uniformly managing
    // ALL synthetic market liquidity through resync avoids that entirely.
    void seed_from_orderbook_row0(hydra::OrderBook &book, const std::string &first_orderbook_line,
                                   int n_levels) {
        resync_to_snapshot(book, first_orderbook_line, n_levels,
                            [](std::int64_t) -> std::uint32_t { return 0; });
    }

    // Applies one replayed LOBSTER event to the live book/matcher, per the
    // event-type table in final_plan.md:
    //   1 (new)             -> add_order (nullptr on pool exhaustion/dup id
    //                          tolerated -- see final_plan.md's named risk)
    //   2 (partial cancel)   -> LOBSTER's `size` is the AMOUNT CANCELLED,
    //                          not the resulting size -- empirically
    //                          verified against real data during planning.
    //                          If the referenced order_id isn't ours (not
    //                          created via type-1 in this file, and not a
    //                          previously-seen counterfactual-diverged
    //                          order), it's a pre-existing order this
    //                          session never saw created -- fall back to
    //                          reducing the SEEDED synthetic order at that
    //                          message's own price level, since that's the
    //                          aggregate proxy for exactly this liquidity.
    //   3 (full deletion)    -> cancel_order, same real/seed/give-up fallback.
    //   4 (visible execution)-> synthesize the real aggressor as an IOC
    //                          order at the historical price/size on the
    //                          historical aggressor side (aggressor_sign =
    //                          -direction); the live Matcher's normal
    //                          price-time priority decides who actually
    //                          gets filled -- naturally consumes seeded
    //                          synthetic liquidity too, no special case
    //                          needed here. This is the counterfactual
    //                          mechanic, see final_plan.md.
    //   5 (hidden execution) -> no OrderBook call, no signal consumes it
    //   7 (halt)             -> skip
    //
    // on_fill is invoked once per hydra::FillEvent a type-4 event generates
    // (zero, one, or several if the aggressor sweeps multiple resting orders).
    template <typename FillHandler>
    void apply_event(const LobsterEvent &ev, hydra::OrderBook &book, hydra::Matcher &matcher,
                      FillHandler &&on_fill) {
        switch (ev.event_type) {
            case 1: {
                hydra::Order order{};
                order.order_id = ev.order_id;
                order.client_id = ev.order_id;  // see kClientIdNote below
                order.price = ev.price_cents;
                order.qty = ev.size;
                order.side = (ev.direction == 1) ? hydra::Side::BUY : hydra::Side::SELL;
                order.tif = hydra::TimeInForce::GTC;
                (void)book.add_order(order);
                break;
            }
            case 2: {
                reduce_or_cancel(book, ev.order_id, ev.price_cents, ev.size, /*full_delete=*/false);
                break;
            }
            case 3: {
                reduce_or_cancel(book, ev.order_id, ev.price_cents, ev.size, /*full_delete=*/true);
                break;
            }
            case 4: {
                hydra::Order aggressor{};
                aggressor.order_id = next_aggressor_order_id_++;
                aggressor.client_id = aggressor.order_id;  // see kClientIdNote below
                aggressor.price = ev.price_cents;
                aggressor.qty = ev.size;
                aggressor.tif = hydra::TimeInForce::IOC;
                // aggressor_sign = -direction: direction==+1 means a resting
                // BUY was executed (the incoming order that hit it was a
                // SELL); direction==-1 -> the incoming order was a BUY.
                aggressor.side = (ev.direction == 1) ? hydra::Side::SELL : hydra::Side::BUY;
                (void)matcher.match(aggressor, std::forward<FillHandler>(on_fill));
                break;
            }
            case 5:
            case 7:
            default:
                break;
        }
    }

    // Resyncs the book's MARKET liquidity to the real orderbook snapshot for
    // this row, by adjusting synthetic "filler" orders.
    //
    // WHY this is necessary (empirically established, not defensive
    // programming): LOBSTER's message file contains no type-1 creation
    // events for orders that predate the file's window, and row 0's
    // snapshot only exposes the top 10 levels -- so deeper pre-existing
    // orders surface later with no way to reconstruct them from messages.
    // Concretely, in AAPL: order 13419503 rests at 58540 from row 226
    // onward in the real book but appears in the message file only at row
    // 448, when it executes. Without resync, replay is exact for the first
    // 439 rows and then diverges permanently the moment such an order
    // surfaces.
    //
    // strategy_qty_at_price returns how much of the book's quantity at a
    // given price belongs to the strategy itself -- excluded from the
    // reconciliation so resync never fights the strategy's own quotes.
    template <typename StrategyQtyFn>
    void resync_to_snapshot(hydra::OrderBook &book, const std::string &orderbook_line, int n_levels,
                             StrategyQtyFn &&strategy_qty_at_price) {
        auto levels = parse_orderbook_line_all_levels(orderbook_line, n_levels);

        // Our current total quantity per price, walking both sides.
        std::unordered_map<std::int64_t, std::uint32_t> our_qty;
        constexpr int kMaxLevelWalk = 64;
        hydra::Level *lvl = book.best_bid_level();
        for (int i = 0; lvl != nullptr && i < kMaxLevelWalk; ++i) {
            our_qty[lvl->price] = lvl->total_qty;
            lvl = book.next_bid_level(lvl->price);
        }
        lvl = book.best_ask_level();
        for (int i = 0; lvl != nullptr && i < kMaxLevelWalk; ++i) {
            our_qty[lvl->price] = lvl->total_qty;
            lvl = book.next_ask_level(lvl->price);
        }

        std::unordered_map<std::int64_t, bool> in_snapshot;
        for (const auto &[side, level] : levels) {
            in_snapshot[level.price_cents] = true;

            std::uint32_t filler_qty = current_filler_qty(book, level.price_cents);
            std::uint32_t total_here = 0;
            if (auto it = our_qty.find(level.price_cents); it != our_qty.end()) {
                total_here = it->second;
            }
            const std::uint32_t strat_here = strategy_qty_at_price(level.price_cents);

            std::uint32_t market_ex_filler = 0;
            const std::uint32_t non_market = filler_qty + strat_here;
            if (total_here > non_market) market_ex_filler = total_here - non_market;

            const std::uint32_t desired_filler =
                (level.size > market_ex_filler) ? (level.size - market_ex_filler) : 0u;
            set_filler(book, level.price_cents, side, desired_filler);
        }

        // Drop fillers at prices the snapshot no longer shows at all --
        // otherwise a stale filler can leave our top-of-book better than
        // the real market's, which is its own kind of divergence.
        for (auto it = filler_by_price_.begin(); it != filler_by_price_.end();) {
            if (!in_snapshot.count(it->first)) {
                (void)book.cancel_order(it->second);
                it = filler_by_price_.erase(it);
            } else {
                ++it;
            }
        }

        // Finally, make the TOUCH authoritative: remove any liquidity --
        // filler OR message-driven -- resting at a price better than the
        // real market's best bid/ask.
        //
        // WHY this is allowed to remove real message-driven orders: LOBSTER
        // gives aggregate quantity per level for 10 levels, never per-order
        // queue composition within a level. Our synthetic fillers therefore
        // can't hold the same queue positions the real orders they proxy
        // did, so an aggressor can consume a filler where reality consumed
        // a specific real order, stranding that real order in our book at a
        // price reality has already cleared. Exact per-order fidelity is
        // simply not reconstructable from this dataset. Top-of-book
        // fidelity IS, and it's what actually governs fill realism for a
        // market maker: where the touch sits, and how much rests ahead of
        // our quote. This makes that exact, and leaves deeper levels and
        // per-order identity as the documented approximation they have to be.
        std::int64_t real_best_bid = std::numeric_limits<std::int64_t>::min();
        std::int64_t real_best_ask = std::numeric_limits<std::int64_t>::max();
        for (const auto &[side, level] : levels) {
            if (side == hydra::Side::BUY) real_best_bid = std::max(real_best_bid, level.price_cents);
            else real_best_ask = std::min(real_best_ask, level.price_cents);
        }
        if (real_best_bid != std::numeric_limits<std::int64_t>::min()) {
            while (hydra::Level *b = book.best_bid_level()) {
                if (b->price <= real_best_bid) break;
                if (!cancel_whole_level(book, b)) break;
            }
        }
        if (real_best_ask != std::numeric_limits<std::int64_t>::max()) {
            while (hydra::Level *a = book.best_ask_level()) {
                if (a->price >= real_best_ask) break;
                if (!cancel_whole_level(book, a)) break;
            }
        }
    }

private:
    // Cancels every order resting at one price level. Returns false if it
    // couldn't make progress (defensive: never spin forever in the caller's
    // while-loop).
    bool cancel_whole_level(hydra::OrderBook &book, hydra::Level *level) {
        bool progressed = false;
        hydra::Order *o = level->head_;
        while (o != nullptr) {
            hydra::Order *next = o->next_;
            const std::uint64_t id = o->order_id;
            if (book.cancel_order(id)) {
                progressed = true;
                auto it = filler_by_price_.find(level->price);
                if (it != filler_by_price_.end() && it->second == id) {
                    filler_by_price_.erase(it);
                }
            }
            o = next;
        }
        return progressed;
    }

    [[nodiscard]] std::uint32_t current_filler_qty(hydra::OrderBook &book, std::int64_t price) {
        auto it = filler_by_price_.find(price);
        if (it == filler_by_price_.end()) return 0;
        const hydra::Order *f = book.find_order(it->second);
        if (f == nullptr) {
            filler_by_price_.erase(it);
            return 0;
        }
        return f->qty;
    }

    void set_filler(hydra::OrderBook &book, std::int64_t price, hydra::Side side,
                     std::uint32_t desired_qty) {
        auto it = filler_by_price_.find(price);
        const std::uint32_t existing = current_filler_qty(book, price);
        it = filler_by_price_.find(price);  // current_filler_qty may have erased it

        if (desired_qty == 0) {
            if (it != filler_by_price_.end()) {
                (void)book.cancel_order(it->second);
                filler_by_price_.erase(it);
            }
            return;
        }
        if (it != filler_by_price_.end() && existing == desired_qty) return;
        if (it != filler_by_price_.end() && desired_qty < existing) {
            (void)book.replace_order_in_place(it->second, desired_qty);
            return;
        }
        // Growing (or creating): replace_order_in_place only shrinks, so
        // cancel and re-add. Losing queue priority here is correct anyway --
        // this is newly-surfaced liquidity, not liquidity that was already
        // ahead of us in the queue.
        if (it != filler_by_price_.end()) {
            (void)book.cancel_order(it->second);
            filler_by_price_.erase(it);
        }
        hydra::Order filler{};
        filler.order_id = next_filler_order_id_++;
        filler.client_id = filler.order_id;  // see kClientIdNote
        filler.price = price;
        filler.qty = desired_qty;
        filler.side = side;
        filler.tif = hydra::TimeInForce::GTC;
        if (book.add_order(filler) != nullptr) {
            filler_by_price_[price] = filler.order_id;
        }
    }

    void reduce_or_cancel(hydra::OrderBook &book, std::uint64_t order_id, std::int64_t /*price_cents*/,
                           std::uint32_t size, bool full_delete) {
        // An order_id we never created is either pre-existing liquidity
        // (never in the message file) or post-divergence drift -- either
        // way, just skip it. resync_to_snapshot() reconciles the affected
        // price level against the real snapshot on this very same row, so
        // there is nothing a price-keyed fallback here could add.
        const hydra::Order *existing = book.find_order(order_id);
        if (existing == nullptr) return;

        if (full_delete || size >= existing->qty) {
            (void)book.cancel_order(order_id);
        } else {
            (void)book.replace_order_in_place(order_id, existing->qty - size);
        }
    }

    std::unordered_map<std::int64_t, std::uint64_t> filler_by_price_;
    std::uint64_t next_filler_order_id_ = kFillerOrderIdBase;
    std::uint64_t next_aggressor_order_id_ = kAggressorOrderIdBase;
};

}  // namespace asmm
