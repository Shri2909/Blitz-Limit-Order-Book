#pragma once

// Order-book fixtures and helpers shared by the book, matcher, pipeline and benchmark tests.

#include "hydra/matcher.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace hydra::test
{

    // client_id defaults to the order id so orders never self-trade unless a test asks for it.
    inline Order make_order(uint64_t id, int64_t price, uint32_t qty, Side side,
                            TimeInForce tif = TimeInForce::GTC, uint64_t client_id = 0)
    {
        Order o{};
        o.order_id = id;
        o.price = price;
        o.qty = qty;
        o.side = side;
        o.tif = tif;
        o.client_id = (client_id == 0) ? id : client_id;
        return o;
    }

    struct FillRecord
    {
        uint64_t maker_order_id;
        int64_t price;
        uint32_t qty;
    };

    // Returns a fill handler that appends every fill to `fills`.
    inline auto record_fills(std::vector<FillRecord> &fills)
    {
        return [&fills](const FillEvent &ev)
        {
            fills.push_back({ev.maker_order_id, ev.price, ev.qty});
        };
    }

    [[nodiscard]] inline uint32_t find_fill_qty(const std::vector<FillRecord> &fills,
                                                uint64_t maker_id)
    {
        for (const auto &f : fills)
        {
            if (f.maker_order_id == maker_id)
            {
                return f.qty;
            }
        }
        return 0;
    }

    // Heap-allocated pools, book and matcher (the pools are too large for the stack).
    struct Fixture
    {
        std::unique_ptr<ObjectPool<Order, ORDER_POOL_SIZE>> order_pool =
            std::make_unique<ObjectPool<Order, ORDER_POOL_SIZE>>();
        std::unique_ptr<ObjectPool<Level, LEVEL_POOL_SIZE>> level_pool =
            std::make_unique<ObjectPool<Level, LEVEL_POOL_SIZE>>();
        std::unique_ptr<OrderBook> book = std::make_unique<OrderBook>(*order_pool, *level_pool);
        std::unique_ptr<Matcher> matcher =
            std::make_unique<Matcher>(*book, MatchingMode::PRICE_TIME);
    };

} // namespace hydra::test
