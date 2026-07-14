// metrics/src/fill_distribution.cpp
//
// Metric A2: price-time vs. pro-rata fill-distribution proof.
//
// Feeds the IDENTICAL starting book state (two resting SELL orders, 30 and
// 70 units, at the same price) through both matching modes against the
// same incoming BUY order (50 units), and reports how each mode actually
// allocates the fill -- strict FIFO priority (price-time) vs. proportional
// split (pro-rata). Both distributions are individually correct; the point
// is that they differ, on the exact same inputs, precisely as designed.
//
// Output: CSV to stdout -- mode,maker_order_id,resting_qty,filled_qty

#include "hydra/matcher.hpp"

#include <cstdio>
#include <memory>

using namespace hydra;

namespace
{

    Order make_order(uint64_t id, int64_t price, uint32_t qty, Side side,
                     TimeInForce tif = TimeInForce::GTC)
    {
        Order o{};
        o.order_id = id;
        o.price = price;
        o.qty = qty;
        o.side = side;
        o.tif = tif;
        o.client_id = id;
        return o;
    }

    void run_mode(const char *mode_name, MatchingMode mode)
    {
        auto order_pool = std::make_unique<ObjectPool<Order, ORDER_POOL_SIZE>>();
        auto level_pool = std::make_unique<ObjectPool<Level, LEVEL_POOL_SIZE>>();
        auto fill_pool = std::make_unique<ObjectPool<FillEvent, FILL_EVENT_POOL_SIZE>>();
        auto book = std::make_unique<OrderBook>(*order_pool, *level_pool);
        auto matcher = std::make_unique<Matcher>(*book, *fill_pool, mode);

        (void)book->add_order(make_order(1, 100, 30, Side::SELL));
        (void)book->add_order(make_order(2, 100, 70, Side::SELL));

        (void)matcher->match(make_order(10, 100, 50, Side::BUY), [&](const FillEvent &ev)
                             { std::printf("%s,%llu,%s,%u\n", mode_name,
                                          static_cast<unsigned long long>(ev.maker_order_id),
                                          ev.maker_order_id == 1 ? "30" : "70", ev.qty); });
    }

} // namespace

int main()
{
    std::printf("mode,maker_order_id,resting_qty,filled_qty\n");
    run_mode("price_time", MatchingMode::PRICE_TIME);
    run_mode("pro_rata", MatchingMode::PRO_RATA);
    return 0;
}
