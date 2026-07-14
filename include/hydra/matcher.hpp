#pragma once

// include/hydra/matcher.hpp
//
// Turns incoming aggressive orders into fills against the resting book, in
// two selectable allocation modes (price-time / pro-rata). Owns no memory
// of its own -- references OrderBook and an ObjectPool<FillEvent,...>
// handed to it at construction (ownership/wiring policy), same as
// order_book.hpp's own OrderBook.

#include "hydra/config.hpp"
#include "hydra/object_pool.hpp"
#include "hydra/order_book.hpp"
#include "hydra/types.hpp"

#include <atomic>
#include <cstdint>

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
        Matcher(OrderBook &book, ObjectPool<FillEvent, FILL_EVENT_POOL_SIZE> &fill_pool,
                MatchingMode initial_mode) noexcept
            : book_(book), fill_pool_(fill_pool), mode_(initial_mode) {}

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

        [[nodiscard]] uint64_t fill_pool_exhaustion_count() const noexcept
        {
            return fill_pool_exhaustion_count_.load(std::memory_order_relaxed);
        }

        template <typename FillHandler>
        [[nodiscard]] uint32_t match(Order incoming, FillHandler &&on_fill) noexcept
        {
            const MatchingMode mode = mode_.load(std::memory_order_relaxed);

            if (incoming.tif == TimeInForce::FOK)
            {
                const uint32_t available = (mode == MatchingMode::PRICE_TIME)
                                               ? available_price_time(incoming)
                                               : available_pro_rata(incoming);
                if (available < incoming.qty) [[unlikely]]
                {
                    return 0;
                }
            }

            uint32_t remaining = incoming.qty;
            uint32_t fills_generated = 0;
            if (mode == MatchingMode::PRICE_TIME)
            {
                sweep_price_time<false>(incoming, remaining, fills_generated, on_fill);
            }
            else
            {
                pro_rata_sweep<false>(incoming, remaining, fills_generated, on_fill);
            }

            if (incoming.tif == TimeInForce::GTC && remaining > 0) [[unlikely]]
            {
                incoming.qty = remaining;
                [[maybe_unused]] Order *rested = book_.add_order(incoming);
            }

            return fills_generated;
        }

    private:
        template <typename FillHandler>
        void apply_fill(Order *resting, Level *level, uint32_t fill_qty,
                        const Order &incoming, uint32_t &remaining,
                        uint32_t &fills_generated, FillHandler &&on_fill) noexcept
        {
            FillEvent fill_storage{};
            fill_storage.maker_order_id = resting->order_id;
            fill_storage.taker_order_id = incoming.order_id;
            fill_storage.price = resting->price;
            fill_storage.qty = fill_qty;
            fill_storage.timestamp_ns = incoming.timestamp_ns;

            // Every executed trade must reach the caller, even if the
            // FillEvent pool is momentarily exhausted -- silently executing a
            // trade with no corresponding event would be a lost fill with no
            // error signal beyond an opaque counter. The pool is used when
            // available (kept for parity with the other pooled types and for
            // the exhaustion telemetry below); local storage is the fallback
            // delivery path, never a dropped delivery.
            FillEvent *fill = fill_pool_.acquire();
            if (fill != nullptr) [[likely]]
            {
                *fill = fill_storage;
                on_fill(*fill);
                fill_pool_.release(fill);
            }
            else [[unlikely]]
            {
                fill_pool_exhaustion_count_.fetch_add(1, std::memory_order_relaxed);
                on_fill(fill_storage);
            }
            ++fills_generated;

            remaining -= fill_qty;
            resting->qty -= fill_qty;
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

        template <bool DryRun, typename FillHandler>
        void sweep_price_time(const Order &incoming, uint32_t &remaining,
                              uint32_t &fills_generated, FillHandler &&on_fill) noexcept
        {
            Level *level = (incoming.side == Side::BUY) ? book_.best_ask_level()
                                                        : book_.best_bid_level();

            while (level != nullptr && remaining > 0 && crosses(level, incoming))
            {
                Level *const next_level = (incoming.side == Side::BUY)
                                              ? book_.next_ask_level(level->price)
                                              : book_.next_bid_level(level->price);

                Order *next_resting = nullptr;
                for (Order *resting = level->head_; resting != nullptr && remaining > 0;
                     resting = next_resting)
                {
                    next_resting = resting->next_;

                    if (OrderBook::is_self_trade(*resting, incoming)) [[unlikely]]
                    {
                        continue;
                    }

                    const uint32_t fill_qty = std::min(remaining, resting->qty);
                    if constexpr (DryRun)
                    {
                        remaining -= fill_qty;
                    }
                    else
                    {
                        apply_fill(resting, level, fill_qty, incoming, remaining,
                                   fills_generated, on_fill);
                    }
                }

                level = next_level;
            }
        }

        [[nodiscard]] uint32_t available_price_time(const Order &incoming) noexcept
        {
            uint32_t remaining = incoming.qty;
            uint32_t unused_fill_count = 0;
            sweep_price_time<true>(incoming, remaining, unused_fill_count,
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
                               uint32_t &fills_generated, FillHandler &&on_fill) noexcept
        {
            Order *const fifo_head = level->head_;

            uint32_t eligible_total = 0;
            Order *largest = nullptr;
            for (Order *r = fifo_head; r != nullptr; r = r->next_)
            {
                if (!OrderBook::is_self_trade(*r, incoming))
                {
                    eligible_total += r->qty;
                    if (largest == nullptr || r->qty > largest->qty)
                    {
                        largest = r;
                    }
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
                                   fills_generated, on_fill);
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
                               fills_generated, on_fill);
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
                            uint32_t &fills_generated, FillHandler &&on_fill) noexcept
        {
            Level *level = (incoming.side == Side::BUY) ? book_.best_ask_level()
                                                        : book_.best_bid_level();

            while (level != nullptr && remaining > 0 && crosses(level, incoming))
            {
                Level *const next_level = (incoming.side == Side::BUY)
                                              ? book_.next_ask_level(level->price)
                                              : book_.next_bid_level(level->price);

                pro_rata_at_level<DryRun>(level, incoming, remaining, fills_generated, on_fill);

                level = next_level;
            }
        }

        [[nodiscard]] uint32_t available_pro_rata(const Order &incoming) noexcept
        {
            uint32_t remaining = incoming.qty;
            uint32_t unused_fill_count = 0;
            pro_rata_sweep<true>(incoming, remaining, unused_fill_count,
                                 [](const FillEvent &) noexcept {});
            return incoming.qty - remaining;
        }

        OrderBook &book_;
        ObjectPool<FillEvent, FILL_EVENT_POOL_SIZE> &fill_pool_;
        std::atomic<MatchingMode> mode_;
        std::atomic<uint64_t> fill_pool_exhaustion_count_{0};
    };

} // namespace hydra