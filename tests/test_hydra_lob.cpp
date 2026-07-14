//===----------------------------------------------------------------------===
// tests/test_hydra_lob.cpp
//
// Standalone correctness suite for OrderBook/Matcher/ObjectPool.
//===----------------------------------------------------------------------===

#include "hydra/matcher.hpp"
#include "hydra/object_pool.hpp"
#include "hydra/order_book.hpp"
#include "hydra/types.hpp"

#include <cstdio>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace hydra::test
{
    namespace
    {

        struct CheckFailure
        {
            std::string message;
        };

#define HYDRA_CHECK(cond)                                            \
    do                                                               \
    {                                                                \
        if (!(cond))                                                 \
        {                                                            \
            std::ostringstream _hydra_oss;                           \
            _hydra_oss << "CHECK(" #cond ") failed at " __FILE__ ":" \
                       << __LINE__;                                  \
            throw ::hydra::test::CheckFailure{_hydra_oss.str()};     \
        }                                                            \
    } while (0)

#define HYDRA_CHECK_EQ(actual, expected)                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        auto _hydra_actual = (actual);                                                                                \
        auto _hydra_expected = (expected);                                                                            \
        if (!(_hydra_actual == _hydra_expected))                                                                      \
        {                                                                                                             \
            std::ostringstream _hydra_oss;                                                                            \
            _hydra_oss << "CHECK_EQ(" #actual ", " #expected ") failed at " __FILE__ ":" << __LINE__ << " -- actual=" \
                       << _hydra_actual << ", expected=" << _hydra_expected;                                          \
            throw ::hydra::test::CheckFailure{_hydra_oss.str()};                                                      \
        }                                                                                                             \
    } while (0)

        int g_pass_count = 0;
        int g_fail_count = 0;

#define RUN_TEST(fn)                                                          \
    do                                                                        \
    {                                                                         \
        try                                                                   \
        {                                                                     \
            fn();                                                             \
            std::fprintf(stderr, "[PASS] %s\n", #fn);                         \
            ++::hydra::test::g_pass_count;                                    \
        }                                                                     \
        catch (const ::hydra::test::CheckFailure &cf)                         \
        {                                                                     \
            std::fprintf(stderr, "[FAIL] %s: %s\n", #fn, cf.message.c_str()); \
            ++::hydra::test::g_fail_count;                                    \
        }                                                                     \
        catch (const std::exception &e)                                       \
        {                                                                     \
            std::fprintf(stderr, "[FAIL] %s: unexpected exception: %s\n",     \
                         #fn, e.what());                                      \
            ++::hydra::test::g_fail_count;                                    \
        }                                                                     \
        catch (...)                                                           \
        {                                                                     \
            std::fprintf(stderr, "[FAIL] %s: unexpected non-exception "       \
                                 "failure\n",                                 \
                         #fn);                                                \
            ++::hydra::test::g_fail_count;                                    \
        }                                                                     \
    } while (0)

        struct Fixture
        {
            std::unique_ptr<ObjectPool<Order, ORDER_POOL_SIZE>> order_pool =
                std::make_unique<ObjectPool<Order, ORDER_POOL_SIZE>>();
            std::unique_ptr<ObjectPool<Level, LEVEL_POOL_SIZE>> level_pool =
                std::make_unique<ObjectPool<Level, LEVEL_POOL_SIZE>>();
            std::unique_ptr<ObjectPool<FillEvent, FILL_EVENT_POOL_SIZE>> fill_pool =
                std::make_unique<ObjectPool<FillEvent, FILL_EVENT_POOL_SIZE>>();
            std::unique_ptr<OrderBook> book =
                std::make_unique<OrderBook>(*order_pool, *level_pool);
            std::unique_ptr<Matcher> matcher = std::make_unique<Matcher>(
                *book, *fill_pool, MatchingMode::PRICE_TIME);
        };

        Order make_order(uint64_t id, int64_t price, uint32_t qty, Side side,
                         TimeInForce tif = TimeInForce::GTC,
                         uint64_t client_id = 0)
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

        void test_add_order()
        {
            Fixture f;

            Order *bid = f.book->add_order(make_order(1, 100, 10, Side::BUY));
            HYDRA_CHECK(bid != nullptr);
            HYDRA_CHECK_EQ(f.book->best_bid().value(), 100);
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{1});
            HYDRA_CHECK(!f.book->best_ask().has_value());

            Order *ask = f.book->add_order(make_order(2, 105, 5, Side::SELL));
            HYDRA_CHECK(ask != nullptr);
            HYDRA_CHECK_EQ(f.book->best_ask().value(), 105);
            HYDRA_CHECK_EQ(f.book->ask_level_count(), std::size_t{1});

            HYDRA_CHECK_EQ(bid->order_id, uint64_t{1});
            HYDRA_CHECK_EQ(bid->qty, uint32_t{10});
            HYDRA_CHECK(bid->side == Side::BUY);

            Order *bid2 = f.book->add_order(make_order(3, 99, 4, Side::BUY));
            HYDRA_CHECK(bid2 != nullptr);
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{2});
            HYDRA_CHECK_EQ(f.book->best_bid().value(), 100);
        }

        void test_cancel_order()
        {
            Fixture f;

            Order *o1 = f.book->add_order(make_order(1, 100, 5, Side::BUY));
            Order *o2 = f.book->add_order(make_order(2, 100, 3, Side::BUY));
            HYDRA_CHECK(o1 != nullptr && o2 != nullptr);
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{1});

            HYDRA_CHECK(f.book->cancel_order(1));
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{1});
            HYDRA_CHECK_EQ(f.book->best_bid().value(), 100);
            Level *lvl = f.book->best_bid_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->order_count, uint32_t{1});
            HYDRA_CHECK_EQ(lvl->total_qty, uint32_t{3});

            HYDRA_CHECK(f.book->cancel_order(2));
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{0});
            HYDRA_CHECK(!f.book->best_bid().has_value());

            HYDRA_CHECK(!f.book->cancel_order(1));
            HYDRA_CHECK(!f.book->cancel_order(999));
        }

        void test_match_full()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));

            std::vector<FillRecord> fills;
            const uint32_t n = f.matcher->match(
                make_order(10, 100, 10, Side::BUY), [&](const FillEvent &ev)
                { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });

            HYDRA_CHECK_EQ(n, uint32_t{1});
            HYDRA_CHECK_EQ(fills.size(), std::size_t{1});
            HYDRA_CHECK_EQ(fills[0].qty, uint32_t{10});
            HYDRA_CHECK(!f.book->best_ask().has_value());
            HYDRA_CHECK(!f.book->best_bid().has_value());
        }

        void test_match_partial()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));

            const uint32_t n = f.matcher->match(make_order(10, 100, 4, Side::BUY),
                                                [](const FillEvent &) {});
            HYDRA_CHECK_EQ(n, uint32_t{1});

            HYDRA_CHECK(f.book->best_ask().has_value());
            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK(lvl->head_ != nullptr);
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{1});
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{6});
            HYDRA_CHECK_EQ(lvl->total_qty, uint32_t{6});
        }

        void test_match_ioc()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 3, Side::SELL));

            const uint32_t n = f.matcher->match(
                make_order(10, 100, 10, Side::BUY, TimeInForce::IOC), [](const FillEvent &) {});
            HYDRA_CHECK_EQ(n, uint32_t{1});

            HYDRA_CHECK(!f.book->best_bid().has_value());
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{0});
        }

        void test_match_fok()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 3, Side::SELL));

            bool handler_called = false;
            const uint32_t n = f.matcher->match(
                make_order(10, 100, 10, Side::BUY, TimeInForce::FOK),
                [&](const FillEvent &)
                { handler_called = true; });

            HYDRA_CHECK_EQ(n, uint32_t{0});
            HYDRA_CHECK(!handler_called);
            HYDRA_CHECK(f.book->best_ask().has_value());
            HYDRA_CHECK_EQ(f.book->best_ask().value(), 100);
            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{3});
            HYDRA_CHECK(!f.book->best_bid().has_value());
        }

        void test_fifo_ordering()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 5, Side::SELL));
            (void)f.book->add_order(make_order(2, 100, 5, Side::SELL));

            std::vector<FillRecord> fills;
            const uint32_t n = f.matcher->match(
                make_order(10, 100, 3, Side::BUY), [&](const FillEvent &ev)
                { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });

            HYDRA_CHECK_EQ(n, uint32_t{1});
            HYDRA_CHECK_EQ(fills.size(), std::size_t{1});
            HYDRA_CHECK_EQ(fills[0].maker_order_id, uint64_t{1});
            HYDRA_CHECK_EQ(fills[0].qty, uint32_t{3});

            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{1});
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{2});
            HYDRA_CHECK(lvl->head_->next_ != nullptr);
            HYDRA_CHECK_EQ(lvl->head_->next_->order_id, uint64_t{2});
            HYDRA_CHECK_EQ(lvl->head_->next_->qty, uint32_t{5});
        }

        void test_pro_rata_distribution()
        {
            {
                Fixture f;
                f.matcher->set_mode(MatchingMode::PRO_RATA);
                (void)f.book->add_order(make_order(1, 100, 20, Side::SELL));
                (void)f.book->add_order(make_order(2, 100, 30, Side::SELL));
                (void)f.book->add_order(make_order(3, 100, 50, Side::SELL));

                std::vector<FillRecord> fills;
                const uint32_t n = f.matcher->match(
                    make_order(10, 100, 100, Side::BUY), [&](const FillEvent &ev)
                    { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });
                HYDRA_CHECK_EQ(n, uint32_t{3});
                uint32_t q1 = 0, q2 = 0, q3 = 0;
                for (auto &fr : fills)
                {
                    if (fr.maker_order_id == 1)
                        q1 = fr.qty;
                    if (fr.maker_order_id == 2)
                        q2 = fr.qty;
                    if (fr.maker_order_id == 3)
                        q3 = fr.qty;
                }
                HYDRA_CHECK_EQ(q1, uint32_t{20});
                HYDRA_CHECK_EQ(q2, uint32_t{30});
                HYDRA_CHECK_EQ(q3, uint32_t{50});
            }

            {
                Fixture f;
                f.matcher->set_mode(MatchingMode::PRO_RATA);
                (void)f.book->add_order(make_order(1, 100, 3, Side::SELL));
                (void)f.book->add_order(make_order(2, 100, 1, Side::SELL));
                (void)f.book->add_order(make_order(3, 100, 1, Side::SELL));

                std::vector<FillRecord> fills;
                const uint32_t n = f.matcher->match(
                    make_order(10, 100, 4, Side::BUY), [&](const FillEvent &ev)
                    { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });
                HYDRA_CHECK(n >= 1 && n <= 3);
                uint32_t total = 0;
                for (auto &fr : fills)
                {
                    total += fr.qty;
                    if (fr.maker_order_id == 1)
                    {
                        HYDRA_CHECK(fr.qty <= 3);
                    }
                    else
                    {
                        HYDRA_CHECK(fr.qty <= 1);
                    }
                }
                HYDRA_CHECK_EQ(total, uint32_t{4});
            }
        }

        void test_cross_spread()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 5, Side::SELL));
            (void)f.book->add_order(make_order(2, 101, 5, Side::SELL));
            (void)f.book->add_order(make_order(3, 102, 100, Side::SELL));

            std::vector<FillRecord> fills;
            const uint32_t n = f.matcher->match(
                make_order(10, 102, 12, Side::BUY), [&](const FillEvent &ev)
                { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });

            HYDRA_CHECK_EQ(n, uint32_t{3});
            HYDRA_CHECK_EQ(fills.size(), std::size_t{3});
            HYDRA_CHECK_EQ(fills[0].price, int64_t{100});
            HYDRA_CHECK_EQ(fills[1].price, int64_t{101});
            HYDRA_CHECK_EQ(fills[2].price, int64_t{102});
            HYDRA_CHECK_EQ(fills[0].qty, uint32_t{5});
            HYDRA_CHECK_EQ(fills[1].qty, uint32_t{5});
            HYDRA_CHECK_EQ(fills[2].qty, uint32_t{2});
            HYDRA_CHECK_EQ(f.book->best_ask().value(), 102);
        }

        void test_self_trade_prevention()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 5, Side::SELL, TimeInForce::GTC, 7));
            (void)f.book->add_order(make_order(2, 100, 5, Side::SELL, TimeInForce::GTC, 9));

            std::vector<FillRecord> fills;
            const uint32_t n = f.matcher->match(
                make_order(10, 100, 5, Side::BUY, TimeInForce::GTC, 7),
                [&](const FillEvent &ev)
                {
                    fills.push_back({ev.maker_order_id, ev.price, ev.qty});
                });

            HYDRA_CHECK_EQ(n, uint32_t{1});
            HYDRA_CHECK_EQ(fills.size(), std::size_t{1});
            HYDRA_CHECK_EQ(fills[0].maker_order_id, uint64_t{2});
            HYDRA_CHECK(f.book->best_ask().has_value());
        }

        void test_price_improvement()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 5, Side::SELL));

            std::vector<FillRecord> fills;
            const uint32_t n = f.matcher->match(
                make_order(10, 105, 5, Side::BUY), [&](const FillEvent &ev)
                { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });

            HYDRA_CHECK_EQ(n, uint32_t{1});
            HYDRA_CHECK_EQ(fills.size(), std::size_t{1});
            HYDRA_CHECK_EQ(fills[0].price, int64_t{100});
        }

        void test_empty_book()
        {
            Fixture f;
            HYDRA_CHECK(!f.book->best_bid().has_value());
            HYDRA_CHECK(!f.book->best_ask().has_value());

            bool handler_called = false;
            const uint32_t n = f.matcher->match(
                make_order(1, 100, 10, Side::BUY), [&](const FillEvent &)
                { handler_called = true; });

            HYDRA_CHECK_EQ(n, uint32_t{0});
            HYDRA_CHECK(!handler_called);
            HYDRA_CHECK(f.book->best_bid().has_value());
            HYDRA_CHECK_EQ(f.book->best_bid().value(), 100);
        }

        void test_duplicate_id()
        {
            Fixture f;
            Order *first = f.book->add_order(make_order(1, 100, 5, Side::BUY));
            HYDRA_CHECK(first != nullptr);

            Order *dup = f.book->add_order(make_order(1, 200, 3, Side::BUY));
            HYDRA_CHECK(dup == nullptr);

            HYDRA_CHECK_EQ(f.book->best_bid().value(), 100);
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{1});

            HYDRA_CHECK(f.book->cancel_order(1));
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{0});
        }

        void test_level_exhaustion()
        {
            Fixture f;

            for (std::size_t i = 0; i < LEVEL_POOL_SIZE; ++i)
            {
                Order *r = f.book->add_order(
                    make_order(static_cast<uint64_t>(i + 1), static_cast<int64_t>(i), 1, Side::BUY));
                if (r == nullptr)
                {
                    throw CheckFailure{"add_order() returned nullptr while genuinely "
                                       "still under LEVEL_POOL_SIZE capacity"};
                }
            }
            HYDRA_CHECK_EQ(f.book->bid_level_count(), LEVEL_POOL_SIZE);

            Order *overflow = f.book->add_order(
                make_order(999'999, static_cast<int64_t>(LEVEL_POOL_SIZE), 1, Side::BUY));
            HYDRA_CHECK(overflow == nullptr);
            HYDRA_CHECK_EQ(f.book->bid_level_count(), LEVEL_POOL_SIZE);

            Order *still_works = f.book->add_order(make_order(999'998, 0, 1, Side::BUY));
            HYDRA_CHECK(still_works != nullptr);
        }

        void test_pool_overflow()
        {
            constexpr std::size_t kCapacity = 4;
            ObjectPool<Order, kCapacity> pool;

            HYDRA_CHECK_EQ(pool.exhaustion_count(), uint64_t{0});

            std::vector<Order *> held;
            for (std::size_t i = 0; i < kCapacity; ++i)
            {
                Order *p = pool.acquire();
                HYDRA_CHECK(p != nullptr);
                held.push_back(p);
            }
            HYDRA_CHECK_EQ(pool.exhaustion_count(), uint64_t{0});

            Order *over1 = pool.acquire();
            HYDRA_CHECK(over1 == nullptr);
            HYDRA_CHECK_EQ(pool.exhaustion_count(), uint64_t{1});

            Order *over2 = pool.acquire();
            HYDRA_CHECK(over2 == nullptr);
            HYDRA_CHECK_EQ(pool.exhaustion_count(), uint64_t{2});

            pool.release(held.back());
            held.pop_back();
            Order *reacquired = pool.acquire();
            HYDRA_CHECK(reacquired != nullptr);
            HYDRA_CHECK_EQ(pool.exhaustion_count(), uint64_t{2});

            for (Order *p : held)
            {
                pool.release(p);
            }
            pool.release(reacquired);
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_add_order);
    RUN_TEST(test_cancel_order);
    RUN_TEST(test_match_full);
    RUN_TEST(test_match_partial);
    RUN_TEST(test_match_ioc);
    RUN_TEST(test_match_fok);
    RUN_TEST(test_fifo_ordering);
    RUN_TEST(test_pro_rata_distribution);
    RUN_TEST(test_cross_spread);
    RUN_TEST(test_self_trade_prevention);
    RUN_TEST(test_price_improvement);
    RUN_TEST(test_empty_book);
    RUN_TEST(test_duplicate_id);
    RUN_TEST(test_level_exhaustion);
    RUN_TEST(test_pool_overflow);

    std::fprintf(stderr, "\n%d passed, %d failed (of %d total)\n", g_pass_count,
                 g_fail_count, g_pass_count + g_fail_count);
    return (g_fail_count == 0) ? 0 : 1;
}