#pragma once

// include/hydra/matcher.hpp
//
// Turns incoming aggressive orders into fills against the resting book, in
// two selectable allocation modes (price-time / pro-rata). Owns no memory
// of its own -- references the OrderBook handed to it at construction
// (ownership/wiring policy). FillEvent notifications are delivered
// synchronously to the caller's on_fill callback and never pooled or
// otherwise retained -- see apply_fill()'s own comment for why.

#include "hydra/order_book.hpp"
#include "hydra/types.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>

namespace hydra
{

    enum class MatchingMode
    {
        PRICE_TIME,
        PRO_RATA
    };

    class Matcher
    {
    public:
        Matcher(OrderBook &book, MatchingMode initial_mode) noexcept
            : book_(book), mode_(initial_mode) {}

        Matcher(const Matcher &) = delete;
        Matcher &operator=(const Matcher &) = delete;

        void set_mode(MatchingMode mode) noexcept
        {
            mode_.store(mode, std::memory_order_relaxed);
        }

        [[nodiscard]] MatchingMode mode() const noexcept
        {
            return mode_.load(std::memory_order_relaxed);
        }

        template <typename FillHandler>
        [[nodiscard]] MatchStats match(const Order &incoming, FillHandler &&on_fill) noexcept
        {
            const MatchingMode mode = mode_.load(std::memory_order_relaxed);

            if (incoming.tif == TimeInForce::FOK)
            {
                const uint32_t available = (mode == MatchingMode::PRICE_TIME)
                                               ? available_price_time(incoming)
                                               : available_pro_rata(incoming);
                if (available < incoming.qty) [[unlikely]]
                {
                    // remaining_qty == incoming.qty here (not the
                    // struct-default 0): nothing filled, so the entire
                    // incoming quantity is "still unfilled" -- keeping
                    // remaining_qty's meaning ("unfilled quantity")
                    // consistent across every return path, including this
                    // rejected-FOK one, rather than letting it default to a
                    // value that would misread as "fully filled."
                    MatchStats rejected{};
                    rejected.remaining_qty = incoming.qty;
                    return rejected;
                }
            }

            uint32_t remaining = incoming.qty;
            MatchStats stats{};
            if (mode == MatchingMode::PRICE_TIME)
            {
                sweep_price_time<false>(incoming, remaining, stats, on_fill);
            }
            else
            {
                pro_rata_sweep<false>(incoming, remaining, stats, on_fill);
            }

            if (incoming.tif == TimeInForce::GTC && remaining > 0) [[unlikely]]
            {
                // Mutable copy constructed only here, on the minority path
                // that actually rests a remainder -- every fully-filled,
                // IOC, or rejected-FOK order (the common case) now avoids
                // copying the 128-byte Order at all, since incoming is a
                // const reference above rather than a by-value parameter.
                Order resting_copy = incoming;
                resting_copy.qty = remaining;
                [[maybe_unused]] Order *rested = book_.add_order(resting_copy);
            }

            stats.remaining_qty = remaining;
            return stats;
        }

        // Modifies the resting order named by order_id to (new_price,
        // new_qty). Standard exchange modify-order semantics: a same-price,
        // quantity-decrease-only replace preserves time priority (O(1),
        // in-place -- it cannot newly cross the book, since the order's
        // marketable price didn't move and its size only shrank). Any other
        // replace (price change, or a quantity increase) loses time
        // priority: the old resting order is cancelled and the modified
        // order is re-submitted through the normal matching sweep as a
        // fresh incoming order, since a price/qty change can newly cross
        // the opposite side. Returns an empty MatchStats (no fills) for the
        // priority-preserving path, or the real MatchStats from match() for
        // the priority-losing path. A no-op (empty MatchStats, book
        // untouched) if order_id is not currently resting.
        template <typename FillHandler>
        [[nodiscard]] MatchStats replace(uint64_t order_id, int64_t new_price, uint32_t new_qty,
                                         FillHandler &&on_fill) noexcept
        {
            const Order *const existing = book_.find_order(order_id);
            if (existing == nullptr) [[unlikely]]
            {
                return MatchStats{};
            }

            if (new_price == existing->price && new_qty <= existing->qty)
            {
                [[maybe_unused]] const bool replaced =
                    book_.replace_order_in_place(order_id, new_qty);
                return MatchStats{};
            }

            Order incoming{};
            incoming.order_id = existing->order_id;
            incoming.price = new_price;
            incoming.qty = new_qty;
            incoming.side = existing->side;
            incoming.tif = existing->tif;
            incoming.event_tag = OrderEventTag::NEW_OR_CANCEL;
            incoming.client_id = existing->client_id;
            incoming.timestamp_ns = existing->timestamp_ns;
            std::memcpy(incoming.client_tag, existing->client_tag, sizeof(incoming.client_tag));

            // Discarded intentionally: order_id was just found live in the
            // book above (find_order() succeeded), so cancel_order()
            // succeeding is guaranteed here -- there is no failure mode to
            // react to, matching apply_fill()'s identical convention below.
            [[maybe_unused]] const bool cancelled = book_.cancel_order(order_id);
            return match(incoming, std::forward<FillHandler>(on_fill));
        }

    private:
        template <typename FillHandler>
        void apply_fill(Order *resting, Level *level, uint32_t fill_qty,
                        const Order &incoming, uint32_t &remaining,
                        MatchStats &stats, FillHandler &&on_fill) noexcept
        {
            FillEvent fill_storage{};
            fill_storage.maker_order_id = resting->order_id;
            fill_storage.taker_order_id = incoming.order_id;
            fill_storage.price = resting->price;
            fill_storage.qty = fill_qty;
            fill_storage.timestamp_ns = incoming.timestamp_ns;

            // Delivered directly, never pooled: fill_storage never outlives
            // this call on any path -- on_fill() is always synchronous, and
            // nothing downstream (the live pipeline's no-op handler, or
            // benchmark.cpp's sample recorder) retains a pointer past the
            // callback returning. A pool round-trip here would only add an
            // acquire/copy/release for an object with nothing to gain from
            // pooling -- the same "hot-path cost, zero consumer" shape this
            // project already removed once elsewhere (HdrHistogram).
            on_fill(fill_storage);
            ++stats.fills_generated;

            remaining -= fill_qty;
            resting->qty -= fill_qty;
            // Zeroing resting->qty here, strictly before cancel_order()
            // below, is load-bearing: OrderBook::unlink_from_level() also
            // decrements level->total_qty by order->qty at unlink time, so
            // that decrement must already see 0 by the time cancel_order()
            // runs, or level->total_qty would be double-decremented for a
            // resting order that just got fully filled. See
            // unlink_from_level()'s own comment (order_book.hpp) for the
            // other half of this contract.
            level->total_qty -= fill_qty;
            if (resting->qty == 0)
            {
                // Discarded intentionally: resting->order_id was just found
                // live in the book above, so cancel_order() succeeding is
                // guaranteed here -- there is no failure mode to react to.
                [[maybe_unused]] const bool cancelled = book_.cancel_order(resting->order_id);
            }
        }

        [[nodiscard]] static bool crosses(const Level *level, const Order &incoming) noexcept
        {
            return (incoming.side == Side::BUY) ? (level->price <= incoming.price)
                                                : (level->price >= incoming.price);
        }

        // Not accumulated on the DryRun (FOK-availability) path: `stats` is
        // reserved for the real commit-pass traversal, so a FOK order's
        // pre-check sweep never double-counts levels/orders examined
        // against the same order's later real sweep -- see
        // available_price_time()/available_pro_rata() below, which pass a
        // throwaway local instead of the caller's real MatchStats.
        template <bool DryRun, typename FillHandler>
        void sweep_price_time(const Order &incoming, uint32_t &remaining,
                              MatchStats &stats, FillHandler &&on_fill) noexcept
        {
            Level *level = (incoming.side == Side::BUY) ? book_.best_ask_level()
                                                        : book_.best_bid_level();

            while (level != nullptr && remaining > 0 && crosses(level, incoming))
            {
                Level *const next_level = (incoming.side == Side::BUY)
                                              ? book_.next_ask_level(level->price)
                                              : book_.next_bid_level(level->price);
                if constexpr (!DryRun)
                {
                    ++stats.levels_consumed;
                }

                Order *next_resting = nullptr;
                for (Order *resting = level->head_; resting != nullptr && remaining > 0;
                     resting = next_resting)
                {
                    next_resting = resting->next_;

                    if constexpr (!DryRun)
                    {
                        ++stats.resting_orders_examined;
                    }

                    if (OrderBook::is_self_trade(*resting, incoming)) [[unlikely]]
                    {
                        if constexpr (!DryRun)
                        {
                            ++stats.self_trade_skips;
                        }
                        continue;
                    }

                    if constexpr (!DryRun)
                    {
                        ++stats.eligible_orders_examined;
                    }

                    const uint32_t fill_qty = std::min(remaining, resting->qty);
                    if constexpr (DryRun)
                    {
                        remaining -= fill_qty;
                    }
                    else
                    {
                        apply_fill(resting, level, fill_qty, incoming, remaining,
                                   stats, on_fill);
                    }
                }

                level = next_level;
            }
        }

        [[nodiscard]] uint32_t available_price_time(const Order &incoming) noexcept
        {
            uint32_t remaining = incoming.qty;
            MatchStats unused_stats{};
            sweep_price_time<true>(incoming, remaining, unused_stats,
                                   [](const FillEvent &) noexcept {});
            return incoming.qty - remaining;
        }

        // Pro-rata allocation at a single already-identified price level.
        // Called by pro_rata_sweep() once per crossing level, mirroring
        // sweep_price_time()'s per-level loop body (see pro_rata_sweep()
        // below for why pro-rata must sweep multiple levels, not just the
        // best one).
        template <bool DryRun, typename FillHandler>
        void pro_rata_at_level(Level *level, const Order &incoming, uint32_t &remaining,
                               MatchStats &stats, FillHandler &&on_fill) noexcept
        {
            Order *const fifo_head = level->head_;

            uint32_t eligible_total = 0;
            Order *largest = nullptr;
            for (Order *r = fifo_head; r != nullptr; r = r->next_)
            {
                // Counted here, once per unique resting order per level per
                // match() call, regardless of DryRun -- this is the single
                // pass every order at the level is guaranteed to be visited
                // in exactly once (the later apply-pass below re-visits the
                // same nodes only on the real commit path, which would
                // double-count "examined" if counted there too).
                if constexpr (!DryRun)
                {
                    ++stats.resting_orders_examined;
                }
                if (!OrderBook::is_self_trade(*r, incoming))
                {
                    eligible_total += r->qty;
                    if (largest == nullptr || r->qty > largest->qty)
                    {
                        largest = r;
                    }
                    if constexpr (!DryRun)
                    {
                        ++stats.eligible_orders_examined;
                    }
                }
                else if constexpr (!DryRun)
                {
                    ++stats.self_trade_skips;
                }
            }
            if (eligible_total == 0) [[unlikely]]
            {
                return;
            }

            const uint32_t level_fill_qty = std::min(remaining, eligible_total);
            if constexpr (DryRun)
            {
                remaining -= level_fill_qty;
                return;
            }
            else
            {
                uint32_t total_floor = 0;
                for (Order *r = fifo_head; r != nullptr; r = r->next_)
                {
                    if (!OrderBook::is_self_trade(*r, incoming))
                    {
                        total_floor += static_cast<uint32_t>(
                            (static_cast<uint64_t>(level_fill_qty) * r->qty) / eligible_total);
                    }
                }
                uint32_t residual = level_fill_qty - total_floor;

                // `largest`'s share (floor + residual, headroom-clamped) is
                // computed here as pure arithmetic -- no pool mutation yet --
                // so it keeps priority over the residual regardless of its
                // FIFO position, matching the original allocation policy.
                // The apply_fill() call for `largest` itself is deferred
                // until *after* the traversal below finishes: apply_fill()
                // can unlink and free `largest` (and, if `largest` was the
                // level's only order, the Level itself), and freeing it
                // before the traversal has walked past its node would make
                // the traversal's `r->next_` reads a use-after-free the
                // instant they reached it.
                const uint32_t largest_floor_share = static_cast<uint32_t>(
                    (static_cast<uint64_t>(level_fill_qty) * largest->qty) / eligible_total);
                const uint32_t largest_headroom = largest->qty - largest_floor_share;
                const uint32_t largest_extra = std::min(residual, largest_headroom);
                residual -= largest_extra;
                const uint32_t largest_total_share = largest_floor_share + largest_extra;

                Order *next_r = nullptr;
                for (Order *r = fifo_head; r != nullptr; r = next_r)
                {
                    next_r = r->next_;
                    if (r == largest || OrderBook::is_self_trade(*r, incoming))
                    {
                        continue;
                    }
                    const uint32_t floor_share = static_cast<uint32_t>(
                        (static_cast<uint64_t>(level_fill_qty) * r->qty) / eligible_total);
                    uint32_t extra = 0;
                    if (residual > 0) [[unlikely]]
                    {
                        const uint32_t headroom = r->qty - floor_share;
                        extra = std::min(residual, headroom);
                        residual -= extra;
                    }
                    const uint32_t total_share = floor_share + extra;
                    if (total_share > 0)
                    {
                        apply_fill(r, level, total_share, incoming, remaining,
                                   stats, on_fill);
                    }
                }

                // Safe now: nothing above dereferences `largest` again after
                // this point, and `level` remains valid too -- with `largest`
                // still resting, order_count could not have reached zero
                // during the loop, so this is guaranteed to be the call that
                // may (or may not) finally drain and free the level.
                if (largest_total_share > 0)
                {
                    apply_fill(largest, level, largest_total_share, incoming, remaining,
                               stats, on_fill);
                }
            }
        }

        // Sweeps price levels from best to worst, applying pro-rata
        // allocation at each crossing level in turn, until `remaining`
        // reaches 0 or no further level crosses the incoming order's limit
        // price. A single-level-only version of this (the previous
        // pro_rata_at_best_level()) left marketable quantity beyond the best
        // level's depth unfilled; for GTC orders that leftover then rested
        // back into the book at the incoming order's own limit price with no
        // re-cross check, which could park a resting order at a price that
        // still crosses the opposite side -- an invalid, crossed book.
        template <bool DryRun, typename FillHandler>
        void pro_rata_sweep(const Order &incoming, uint32_t &remaining,
                            MatchStats &stats, FillHandler &&on_fill) noexcept
        {
            Level *level = (incoming.side == Side::BUY) ? book_.best_ask_level()
                                                        : book_.best_bid_level();

            while (level != nullptr && remaining > 0 && crosses(level, incoming))
            {
                Level *const next_level = (incoming.side == Side::BUY)
                                              ? book_.next_ask_level(level->price)
                                              : book_.next_bid_level(level->price);
                if constexpr (!DryRun)
                {
                    ++stats.levels_consumed;
                }

                pro_rata_at_level<DryRun>(level, incoming, remaining, stats, on_fill);

                level = next_level;
            }
        }

        [[nodiscard]] uint32_t available_pro_rata(const Order &incoming) noexcept
        {
            uint32_t remaining = incoming.qty;
            MatchStats unused_stats{};
            pro_rata_sweep<true>(incoming, remaining, unused_stats,
                                 [](const FillEvent &) noexcept {});
            return incoming.qty - remaining;
        }

        OrderBook &book_;
        std::atomic<MatchingMode> mode_;
    };

} // namespace hydra