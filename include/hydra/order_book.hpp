#pragma once

// include/hydra/order_book.hpp
//
// The resting-order state machine: price-level maps, FIFO per level, O(1)
// cancel. Owns no memory of its own -- every Order/Level it touches comes
// from the two pools handed to it at construction (ownership/wiring policy:
// OrderBook stores references, never allocates outside those pools).

#include "hydra/config.hpp"
#include "hydra/object_pool.hpp"
#include "hydra/types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory_resource>
#include <optional>
#include <unordered_map>

namespace hydra
{

    class OrderBook
    {
    public:
        OrderBook(ObjectPool<Order, ORDER_POOL_SIZE> &order_pool,
                  ObjectPool<Level, LEVEL_POOL_SIZE> &level_pool) noexcept
            : order_index_buffer_resource_(order_index_arena_.data(),
                                           order_index_arena_.size(),
                                           &fallback_resource_),
              order_index_pool_resource_(&order_index_buffer_resource_),
              level_map_buffer_resource_(level_map_arena_.data(),
                                         level_map_arena_.size(),
                                         &fallback_resource_),
              level_map_pool_resource_(&level_map_buffer_resource_),
              bids_(std::greater<int64_t>(), &level_map_pool_resource_),
              asks_(&level_map_pool_resource_),
              order_index_(&order_index_pool_resource_),
              order_pool_(order_pool),
              level_pool_(level_pool)
        {
            order_index_.reserve(ORDER_POOL_SIZE);
        }

        OrderBook(const OrderBook &) = delete;
        OrderBook &operator=(const OrderBook &) = delete;

        [[nodiscard]] Order *add_order(const Order &incoming) noexcept
        {
            if (order_index_.find(incoming.order_id) != order_index_.end()) [[unlikely]]
            {
                return nullptr;
            }

            Order *resting = order_pool_.acquire();
            if (resting == nullptr) [[unlikely]]
            {
                return nullptr;
            }
            *resting = incoming;

            Level *level = find_or_create_level(resting->side, resting->price);
            if (level == nullptr) [[unlikely]]
            {
                order_pool_.release(resting);
                return nullptr;
            }

            append_to_level(level, resting);
            order_index_.emplace(resting->order_id, Entry{resting, level});
            return resting;
        }

        [[nodiscard]] bool cancel_order(uint64_t order_id) noexcept
        {
            auto it = order_index_.find(order_id);
            if (it == order_index_.end()) [[unlikely]]
            {
                return false;
            }
            Order *const order = it->second.order;
            Level *const level = it->second.level;
            const Side side = order->side;

            unlink_from_level(level, order);
            order_index_.erase(it);
            order_pool_.release(order);

            if (level->order_count == 0)
            {
                const int64_t price = level->price;
                if (side == Side::BUY)
                {
                    bids_.erase(price);
                }
                else
                {
                    asks_.erase(price);
                }
                level_pool_.release(level);
            }
            return true;
        }

        // Read-only O(1) lookup by order_id -- used by Matcher::replace() to
        // snapshot the resting order's side/tif/client_id/qty/price before
        // deciding which replace path applies (see that function). Returns
        // nullptr for an unknown/already-gone order_id, mirroring
        // cancel_order()'s own "not found is a normal race, not an error"
        // convention.
        [[nodiscard]] const Order *find_order(uint64_t order_id) const noexcept
        {
            const auto it = order_index_.find(order_id);
            return (it == order_index_.end()) ? nullptr : it->second.order;
        }

        // Priority-preserving in-place replace: caller (Matcher::replace())
        // guarantees new_qty <= the order's current qty and that price is
        // unchanged -- under those two conditions the order cannot newly
        // cross the book (its marketable price didn't move and its size
        // only shrank), so no unlink/relink or re-match is needed. O(1):
        // mutates the order's qty and the level's total_qty by the delta in
        // place, exactly preserving the order's FIFO position.
        [[nodiscard]] bool replace_order_in_place(uint64_t order_id, uint32_t new_qty) noexcept
        {
            const auto it = order_index_.find(order_id);
            if (it == order_index_.end()) [[unlikely]]
            {
                return false;
            }
            Order *const order = it->second.order;
            Level *const level = it->second.level;
            const uint32_t delta = order->qty - new_qty;
            order->qty = new_qty;
            level->total_qty -= delta;
            return true;
        }

        [[nodiscard]] std::optional<int64_t> best_bid() const noexcept
        {
            if (bids_.empty()) [[unlikely]]
            {
                return std::nullopt;
            }
            else [[likely]]
            {
                return bids_.begin()->first;
            }
        }

        [[nodiscard]] std::optional<int64_t> best_ask() const noexcept
        {
            if (asks_.empty()) [[unlikely]]
            {
                return std::nullopt;
            }
            else [[likely]]
            {
                return asks_.begin()->first;
            }
        }

        [[nodiscard]] static bool is_self_trade(const Order &resting,
                                                const Order &incoming) noexcept
        {
            return resting.client_id == incoming.client_id;
        }

        [[nodiscard]] Level *best_bid_level() noexcept
        {
            return bids_.empty() ? nullptr : bids_.begin()->second;
        }

        [[nodiscard]] Level *best_ask_level() noexcept
        {
            return asks_.empty() ? nullptr : asks_.begin()->second;
        }

        [[nodiscard]] Level *next_bid_level(int64_t after_price) noexcept
        {
            auto it = bids_.find(after_price);
            if (it == bids_.end()) [[unlikely]]
            {
                return nullptr;
            }
            ++it;
            return (it == bids_.end()) ? nullptr : it->second;
        }

        [[nodiscard]] Level *next_ask_level(int64_t after_price) noexcept
        {
            auto it = asks_.find(after_price);
            if (it == asks_.end()) [[unlikely]]
            {
                return nullptr;
            }
            ++it;
            return (it == asks_.end()) ? nullptr : it->second;
        }

        [[nodiscard]] std::size_t bid_level_count() const noexcept { return bids_.size(); }
        [[nodiscard]] std::size_t ask_level_count() const noexcept { return asks_.size(); }

        [[nodiscard]] uint64_t arena_fallback_count() const noexcept
        {
            return fallback_resource_.fallback_count();
        }

        // Clears all resting orders/levels so a caller can replay an
        // identical event sequence (same order_ids) against a genuinely
        // clean book. WHY this exists: run_benchmark()'s trial loop used to
        // replay the same dataset (fixed order_ids) against this SAME book
        // across all 5 trials with no reset in between -- any order_id
        // still resting at the end of trial N would make add_order() for
        // that same order_id in trial N+1 silently return nullptr (the
        // duplicate-order_id rejection, correct and tested on its own --
        // see test_duplicate_id), a no-op the caller (Matcher::match()'s
        // GTC path) discards without a counter or log line. A rejected
        // add is cheaper than a real one, so those phantom samples'
        // match_time_ns/end_to_end_ns were silently biased low, and
        // trials 2-5 weren't actually independent replays of the same
        // workload. This method, called between trials, is the fix.
        //
        // Only clears order_index_/bids_/asks_ -- does NOT release
        // individual Order*/Level* back to order_pool_/level_pool_ one at
        // a time; the caller is expected to also call
        // order_pool_.reset()/level_pool_.reset() (see benchmark.cpp),
        // which rebuilds those pools' free lists unconditionally and makes
        // an itemized release here redundant.
        //
        // Caller's responsibility, not enforced here: only call this when
        // nothing else is concurrently touching the book (e.g. after
        // confirming the consumer/matching thread has drained and is idle
        // -- see the call site in benchmark.cpp).
        void reset() noexcept
        {
            bids_.clear();
            asks_.clear();
            order_index_.clear();
        }

    private:
        struct Entry
        {
            Order *order;
            Level *level;
        };

        class InstrumentedFallbackResource : public std::pmr::memory_resource
        {
        public:
            [[nodiscard]] uint64_t fallback_count() const noexcept
            {
                return count_.load(std::memory_order_relaxed);
            }

        private:
            void *do_allocate(std::size_t bytes, std::size_t alignment) override
            {
                count_.fetch_add(1, std::memory_order_relaxed);
                return std::pmr::new_delete_resource()->allocate(bytes, alignment);
            }
            void do_deallocate(void *p, std::size_t bytes,
                               std::size_t alignment) override
            {
                std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
            }
            [[nodiscard]] bool do_is_equal(
                const std::pmr::memory_resource &other) const noexcept override
            {
                return this == &other;
            }
            std::atomic<uint64_t> count_{0};
        };

        static constexpr std::size_t kOrderIndexArenaBytes =
            ORDER_POOL_SIZE * 48 + 700'000;
        static constexpr std::size_t kLevelMapArenaBytes =
            LEVEL_POOL_SIZE * 64;

        alignas(alignof(std::max_align_t))
            std::array<std::byte, kOrderIndexArenaBytes> order_index_arena_{};
        alignas(alignof(std::max_align_t))
            std::array<std::byte, kLevelMapArenaBytes> level_map_arena_{};

        InstrumentedFallbackResource fallback_resource_;
        std::pmr::monotonic_buffer_resource order_index_buffer_resource_;
        std::pmr::unsynchronized_pool_resource order_index_pool_resource_;
        std::pmr::monotonic_buffer_resource level_map_buffer_resource_;
        std::pmr::unsynchronized_pool_resource level_map_pool_resource_;

        std::pmr::map<int64_t, Level *, std::greater<int64_t>> bids_;
        std::pmr::map<int64_t, Level *> asks_;
        std::pmr::unordered_map<uint64_t, Entry> order_index_;

        ObjectPool<Order, ORDER_POOL_SIZE> &order_pool_;
        ObjectPool<Level, LEVEL_POOL_SIZE> &level_pool_;

        template <typename PriceMap>
        [[nodiscard]] Level *get_or_create_level(PriceMap &price_map,
                                                 int64_t price) noexcept
        {
            auto it = price_map.find(price);
            if (it != price_map.end()) [[likely]]
            {
                return it->second;
            }
            Level *level = level_pool_.acquire();
            if (level == nullptr) [[unlikely]]
            {
                return nullptr;
            }
            level->price = price;
            level->total_qty = 0;
            level->order_count = 0;
            level->head_ = nullptr;
            level->tail_ = nullptr;
            price_map.emplace(price, level);
            return level;
        }

        [[nodiscard]] Level *find_or_create_level(Side side, int64_t price) noexcept
        {
            return (side == Side::BUY) ? get_or_create_level(bids_, price)
                                       : get_or_create_level(asks_, price);
        }

        static void append_to_level(Level *level, Order *order) noexcept
        {
            order->prev_ = level->tail_;
            order->next_ = nullptr;
            if (level->tail_ != nullptr)
            {
                level->tail_->next_ = order;
            }
            else
            {
                level->head_ = order;
            }
            level->tail_ = order;
            level->total_qty += order->qty;
            ++level->order_count;
        }

        static void unlink_from_level(Level *level, Order *order) noexcept
        {
            if (order->prev_ != nullptr)
            {
                order->prev_->next_ = order->next_;
            }
            else
            {
                level->head_ = order->next_;
            }
            if (order->next_ != nullptr)
            {
                order->next_->prev_ = order->prev_;
            }
            else
            {
                level->tail_ = order->prev_;
            }
            // level->total_qty -= order->qty here assumes order->qty is
            // already the correct "how much this order still contributes"
            // at the moment of unlink. Holds for a genuine external cancel
            // (order->qty is the full untouched resting quantity) and for
            // a matcher-triggered cancel-on-full-fill (Matcher::apply_fill
            // zeroes resting->qty *before* calling cancel_order(), so this
            // line subtracts zero and total_qty was already correctly
            // decremented by apply_fill's own explicit subtraction) -- see
            // apply_fill()'s matching comment (matcher.hpp) for the other
            // half of this contract. Reordering either side would silently
            // double-decrement total_qty for a fully-filled resting order.
            level->total_qty -= order->qty;
            --level->order_count;
        }
    };

} // namespace hydra
