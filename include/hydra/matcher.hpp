#pragma once

// Matches incoming orders using price-time or pro-rata allocation.
// References an external OrderBook; fill callbacks are synchronous.

#include "hydra/order_book.hpp"
#include "hydra/types.hpp"

#include <algorithm>
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
            : book_(book), mode_(initial_mode)
        {
        }

        Matcher(const Matcher &) = delete;
        Matcher &operator=(const Matcher &) = delete;

        void set_mode(MatchingMode mode) noexcept { mode_.store(mode, std::memory_order_relaxed); }

        [[nodiscard]] MatchingMode mode() const noexcept
        {
            return mode_.load(std::memory_order_relaxed);
        }

        template <typename FillHandler>
        [[nodiscard]] MatchStats match(const Order &incoming, FillHandler &&on_fill) noexcept
        {
            // Reject duplicate live IDs before trading. An unchanged remaining_qty
            // reports that no quantity was filled.
            if (book_.find_order(incoming.order_id) != nullptr) [[unlikely]]
            {
                MatchStats rejected{};
                rejected.remaining_qty = incoming.qty;
                return rejected;
            }

            const MatchingMode mode = mode_.load(std::memory_order_relaxed);

            if (incoming.tif == TimeInForce::FOK)
            {
                const uint32_t available = (mode == MatchingMode::PRICE_TIME)
                                               ? available_price_time(incoming)
                                               : available_pro_rata(incoming);
                if (available < incoming.qty) [[unlikely]]
                {
                    // A rejected FOK leaves the entire incoming quantity unfilled.
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
                // Discard a GTC remainder that would cross self-trade-skipped liquidity.
                // Existing resting orders remain in place, and the book stays uncrossed.
                const Level *const opp =
                    (incoming.side == Side::BUY) ? book_.best_ask_level() : book_.best_bid_level();
                const bool would_rest_crossed = (opp != nullptr) && crosses(opp, incoming);
                if (!would_rest_crossed) [[likely]]
                {
                    // Copy the order only when a remainder must rest.
                    Order resting_copy = incoming;
                    resting_copy.qty = remaining;
                    [[maybe_unused]] Order *rested = book_.add_order(resting_copy);
                }
            }

            stats.remaining_qty = remaining;
            return stats;
        }

        // Same-price quantity reductions preserve FIFO priority in place. Price
        // changes or quantity increases cancel and rematch as a fresh order.
        // Zero quantity cancels; an unknown ID is a no-op. Only the rematch path
        // can return fills.
        template <typename FillHandler>
        [[nodiscard]] MatchStats replace(uint64_t order_id, int64_t new_price, uint32_t new_qty,
                                         FillHandler &&on_fill) noexcept
        {
            const Order *const existing = book_.find_order(order_id);
            if (existing == nullptr) [[unlikely]]
            {
                return MatchStats{};
            }

            // A zero-quantity replacement cancels the resting order.
            if (new_qty == 0)
            {
                [[maybe_unused]] const bool cancelled = book_.cancel_order(order_id);
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

            // The ID is known to be live, so cancellation must succeed.
            [[maybe_unused]] const bool cancelled = book_.cancel_order(order_id);
            return match(incoming, std::forward<FillHandler>(on_fill));
        }

    private:
        template <typename FillHandler>
        void apply_fill(Order *resting, Level *level, uint32_t fill_qty, const Order &incoming,
                        uint32_t &remaining, MatchStats &stats, FillHandler &&on_fill) noexcept
        {
            FillEvent fill_storage{};
            fill_storage.maker_order_id = resting->order_id;
            fill_storage.taker_order_id = incoming.order_id;
            fill_storage.price = resting->price;
            fill_storage.qty = fill_qty;
            fill_storage.timestamp_ns = incoming.timestamp_ns;

            // The callback may read the stack-local fill only until it returns.
            on_fill(fill_storage);
            ++stats.fills_generated;

            remaining -= fill_qty;
            resting->qty -= fill_qty;
            // Update quantity before cancellation: unlink_from_level() subtracts the
            // remaining quantity, which must be zero for a fully filled order.
            level->total_qty -= fill_qty;
            if (resting->qty == 0)
            {
                // The filled order is still indexed, so cancellation must succeed.
                [[maybe_unused]] const bool cancelled = book_.cancel_order(resting->order_id);
            }
        }

        [[nodiscard]] static bool crosses(const Level *level, const Order &incoming) noexcept
        {
            return (incoming.side == Side::BUY) ? (level->price <= incoming.price)
                                                : (level->price >= incoming.price);
        }

        // FOK availability checks use DryRun and do not contribute to MatchStats.
        template <bool DryRun, typename FillHandler>
        void sweep_price_time(const Order &incoming, uint32_t &remaining, MatchStats &stats,
                              FillHandler &&on_fill) noexcept
        {
            Level *level =
                (incoming.side == Side::BUY) ? book_.best_ask_level() : book_.best_bid_level();

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
                        apply_fill(resting, level, fill_qty, incoming, remaining, stats, on_fill);
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

        // Allocate fills at one crossing price level using pro-rata shares.
        template <bool DryRun, typename FillHandler>
        void pro_rata_at_level(Level *level, const Order &incoming, uint32_t &remaining,
                               MatchStats &stats, FillHandler &&on_fill) noexcept
        {
            Order *const fifo_head = level->head_;

            // The sum of valid uint32_t quantities can exceed UINT32_MAX.
            uint64_t eligible_total = 0;
            Order *largest = nullptr;
            for (Order *r = fifo_head; r != nullptr; r = r->next_)
            {
                // Count each resting order once per level on the commit path. The later
                // allocation pass revisits the same nodes and must not count them again.
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

            // The fill is bounded by remaining, so it fits in uint32_t.
            const uint32_t level_fill_qty =
                static_cast<uint32_t>(std::min(static_cast<uint64_t>(remaining), eligible_total));
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

                // Reserve the residual for the largest order, capped by its headroom.
                // Apply its fill after traversing the other orders: filling it first could
                // free its node or the level before the traversal finishes.
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
                        apply_fill(r, level, total_share, incoming, remaining, stats, on_fill);
                    }
                }

                // The largest order keeps the level alive until this final fill.
                if (largest_total_share > 0)
                {
                    apply_fill(largest, level, largest_total_share, incoming, remaining, stats,
                               on_fill);
                }
            }
        }

        // Sweep crossing levels from best to worst until the order is filled or
        // its limit price prevents further execution.
        template <bool DryRun, typename FillHandler>
        void pro_rata_sweep(const Order &incoming, uint32_t &remaining, MatchStats &stats,
                            FillHandler &&on_fill) noexcept
        {
            Level *level =
                (incoming.side == Side::BUY) ? book_.best_ask_level() : book_.best_bid_level();

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
