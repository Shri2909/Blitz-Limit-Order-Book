// metrics/src/cancel_latency_sweep.cpp
//
// Metric A1/B4: O(1) cancel_order() proof.
//
// Cancels an order at the FIFO head vs. the FIFO tail of a price level,
// across a sweep of FIFO depths, and reports the average wall-clock cost of
// each. cancel_order() goes straight to its target via the order_index_
// handle (see order_book.hpp) rather than walking the FIFO -- if that ever
// regressed to a linear scan, tail-cancel cost would grow with depth; an
// O(1) cancel keeps both curves flat and roughly equal regardless of depth.
//
// Output: CSV to stdout -- depth,avg_head_ns,avg_tail_ns

#include "hydra/order_book.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace hydra;

namespace
{

    Order make_order(uint64_t id, int64_t price, uint32_t qty, Side side)
    {
        Order o{};
        o.order_id = id;
        o.price = price;
        o.qty = qty;
        o.side = side;
        o.tif = TimeInForce::GTC;
        o.client_id = id;
        return o;
    }

    double measure_avg_ns_at_position(OrderBook &book, uint64_t &next_id, int depth,
                                      int trials, std::size_t target_position)
    {
        double total_ns = 0.0;
        std::vector<uint64_t> ids;
        ids.reserve(static_cast<std::size_t>(depth));

        for (int t = 0; t < trials; ++t)
        {
            ids.clear();
            for (int i = 0; i < depth; ++i)
            {
                const uint64_t id = next_id++;
                Order *r = book.add_order(make_order(id, 100, 1, Side::BUY));
                if (r == nullptr)
                {
                    std::fprintf(stderr, "cancel_latency_sweep: add_order failed unexpectedly "
                                         "at depth=%d (pool exhausted?)\n",
                                 depth);
                    std::exit(1);
                }
                ids.push_back(id);
            }

            const uint64_t target_id = ids[target_position];
            const auto start = std::chrono::steady_clock::now();
            const bool cancelled = book.cancel_order(target_id);
            const auto end = std::chrono::steady_clock::now();
            if (!cancelled)
            {
                std::fprintf(stderr, "cancel_latency_sweep: cancel_order failed unexpectedly\n");
                std::exit(1);
            }
            total_ns += static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());

            for (const uint64_t id : ids)
            {
                if (id != target_id)
                {
                    (void)book.cancel_order(id);
                }
            }
        }
        return total_ns / static_cast<double>(trials);
    }

} // namespace

int main()
{
    auto order_pool = std::make_unique<ObjectPool<Order, ORDER_POOL_SIZE>>();
    auto level_pool = std::make_unique<ObjectPool<Level, LEVEL_POOL_SIZE>>();
    auto book = std::make_unique<OrderBook>(*order_pool, *level_pool);

    const std::vector<int> depths = {100, 500, 1000, 2000, 5000, 10000, 20000, 40000};
    constexpr int kTrials = 50;

    std::printf("depth,avg_head_ns,avg_tail_ns\n");
    uint64_t next_id = 1;
    for (const int depth : depths)
    {
        const double avg_head = measure_avg_ns_at_position(*book, next_id, depth, kTrials, 0);
        const double avg_tail = measure_avg_ns_at_position(
            *book, next_id, depth, kTrials, static_cast<std::size_t>(depth - 1));
        std::printf("%d,%.2f,%.2f\n", depth, avg_head, avg_tail);
        std::fflush(stdout);
    }

    return 0;
}
