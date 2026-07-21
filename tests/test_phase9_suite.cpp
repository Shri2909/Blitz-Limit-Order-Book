//===----------------------------------------------------------------------===
// tests/test_phase9_suite.cpp
//
// Phase 9's canonical 15-case suite lived in test_hydra_lob.cpp per its own
// exit condition ("exactly these 15 named tests") -- that file was the
// retired monolithic suite (see test_phase6_matcher.cpp's own reference to
// it) and no longer exists; its 15 cases were absorbed into the per-phase
// split this suite is now part of. This file is the CMake comment block's
// "extended edge cases beyond the 15 core" -- scenarios the canonical suite
// deliberately doesn't cover (it was fixed at 15 by design), but that this
// codebase's actual behavior should still be pinned down:
// order/level pool exhaustion during the GTC-rest step (not just a direct
// add), multi-level IOC/FOK sweeps, self-trade skipping across a level
// boundary, cross-call partial-fill sequences, and a large-scale
// internal-consistency invariant check.
//===----------------------------------------------------------------------===

#include "hydra/matcher.hpp"

#include "test_harness.hpp"

#include <cstdio>
#include <memory>
#include <vector>

namespace hydra::test
{
    namespace
    {

        Order make_order(uint64_t id, int64_t price, uint32_t qty, Side side,
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

        struct Fixture
        {
            std::unique_ptr<ObjectPool<Order, ORDER_POOL_SIZE>> order_pool =
                std::make_unique<ObjectPool<Order, ORDER_POOL_SIZE>>();
            std::unique_ptr<ObjectPool<Level, LEVEL_POOL_SIZE>> level_pool =
                std::make_unique<ObjectPool<Level, LEVEL_POOL_SIZE>>();
            std::unique_ptr<OrderBook> book =
                std::make_unique<OrderBook>(*order_pool, *level_pool);
            std::unique_ptr<Matcher> matcher = std::make_unique<Matcher>(
                *book, MatchingMode::PRICE_TIME);
        };

        [[nodiscard]] uint32_t find_fill_qty(const std::vector<FillRecord> &fills, uint64_t maker_id)
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

        // Order pool exhaustion specifically during the GTC "rest the
        // leftover" step (not add_order() called directly, as Phase 5
        // already covers). An empty, non-crossing book means nothing gets
        // filled/cancelled during the match itself, so no slot is freed up
        // before the rest attempt -- the pool stays genuinely exhausted
        // when book_.add_order(incoming) tries to acquire one.
        void test_order_pool_exhaustion_during_rest_after_no_liquidity()
        {
            Fixture f;

            std::vector<Order *> held;
            held.reserve(ORDER_POOL_SIZE);
            for (std::size_t i = 0; i < ORDER_POOL_SIZE; ++i)
            {
                Order *o = f.order_pool->acquire();
                HYDRA_CHECK(o != nullptr);
                held.push_back(o);
            }

            const MatchStats n = f.matcher->match(
                make_order(10, 100, 10, Side::BUY), [](const FillEvent &) {});

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{0});
            HYDRA_CHECK(!f.book->best_bid().has_value());
        }

        // Same idea, but for the Level pool: order_pool has normal capacity
        // (so the Order itself acquires fine), but no price level exists
        // yet at the incoming's price and level_pool is fully drained, so
        // find_or_create_level() fails. Confirms OrderBook::add_order()'s
        // own cleanup path (releasing the just-acquired Order back to
        // order_pool_ when level creation fails) doesn't leak.
        void test_level_pool_exhaustion_during_rest_at_new_price()
        {
            Fixture f;

            std::vector<Level *> held;
            held.reserve(LEVEL_POOL_SIZE);
            for (std::size_t i = 0; i < LEVEL_POOL_SIZE; ++i)
            {
                Level *lvl = f.level_pool->acquire();
                HYDRA_CHECK(lvl != nullptr);
                held.push_back(lvl);
            }

            const MatchStats n = f.matcher->match(
                make_order(10, 100, 10, Side::BUY), [](const FillEvent &) {});

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{0});
            HYDRA_CHECK(!f.book->best_bid().has_value());
            // The Order acquired for the failed rest attempt must have been
            // released back, not leaked.
            HYDRA_CHECK_EQ(f.order_pool->exhaustion_count(), uint64_t{0});
        }

        void test_ioc_sweeps_multiple_levels_before_discarding_remainder()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));
            (void)f.book->add_order(make_order(2, 101, 10, Side::SELL));

            std::vector<FillRecord> fills;
            const MatchStats n = f.matcher->match(
                make_order(10, 101, 25, Side::BUY, TimeInForce::IOC), [&](const FillEvent &ev)
                { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{2});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 1), uint32_t{10});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 2), uint32_t{10});
            // Both levels fully drained; 5 units of unfillable IOC quantity
            // discarded, not rested.
            HYDRA_CHECK(!f.book->best_ask().has_value());
            HYDRA_CHECK(!f.book->best_bid().has_value());
        }

        void test_fok_evaluates_availability_across_multiple_levels()
        {
            // Success case: 20 requested, 25 available across two levels.
            {
                Fixture f;
                (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));
                (void)f.book->add_order(make_order(2, 101, 15, Side::SELL));

                std::vector<FillRecord> fills;
                const MatchStats n = f.matcher->match(
                    make_order(10, 101, 20, Side::BUY, TimeInForce::FOK), [&](const FillEvent &ev)
                    { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });

                HYDRA_CHECK_EQ(n.fills_generated, uint32_t{2});
                HYDRA_CHECK_EQ(find_fill_qty(fills, 1), uint32_t{10});
                HYDRA_CHECK_EQ(find_fill_qty(fills, 2), uint32_t{10});
                HYDRA_CHECK(f.book->best_ask().has_value());
                Level *lvl = f.book->best_ask_level();
                HYDRA_CHECK(lvl != nullptr);
                HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{2});
                HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{5});
            }

            // Failure case: 30 requested, only 25 available -- must mutate
            // nothing at all, even though the first level alone had some
            // (insufficient) liquidity.
            {
                Fixture f;
                (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));
                (void)f.book->add_order(make_order(2, 101, 15, Side::SELL));

                bool handler_called = false;
                const MatchStats n = f.matcher->match(
                    make_order(10, 101, 30, Side::BUY, TimeInForce::FOK),
                    [&](const FillEvent &) { handler_called = true; });

                HYDRA_CHECK_EQ(n.fills_generated, uint32_t{0});
                HYDRA_CHECK(!handler_called);
                HYDRA_CHECK_EQ(f.book->best_ask().value(), int64_t{100});
                Level *lvl0 = f.book->best_ask_level();
                HYDRA_CHECK(lvl0 != nullptr);
                HYDRA_CHECK_EQ(lvl0->head_->qty, uint32_t{10});
                Level *lvl1 = f.book->next_ask_level(lvl0->price);
                HYDRA_CHECK(lvl1 != nullptr);
                HYDRA_CHECK_EQ(lvl1->head_->qty, uint32_t{15});
            }
        }

        // Self-trade skip within a level (order1) followed by a sweep that
        // continues into a second order at the SAME level (order2) and then
        // a second level entirely (order3) -- confirms self-trade skipping
        // doesn't stop or corrupt the rest of the sweep.
        void test_self_trade_prevention_across_multiple_levels_in_sweep()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL, TimeInForce::GTC, 7));
            (void)f.book->add_order(make_order(2, 100, 10, Side::SELL, TimeInForce::GTC, 9));
            (void)f.book->add_order(make_order(3, 101, 10, Side::SELL, TimeInForce::GTC, 9));

            std::vector<FillRecord> fills;
            const MatchStats n = f.matcher->match(
                make_order(10, 101, 25, Side::BUY, TimeInForce::GTC, 7),
                [&](const FillEvent &ev)
                { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{2});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 2), uint32_t{10});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 3), uint32_t{10});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 1), uint32_t{0}); // never touched

            // order1 (self-trade) is still resting, untouched, at price 100.
            HYDRA_CHECK(f.book->best_ask().has_value());
            HYDRA_CHECK_EQ(f.book->best_ask().value(), int64_t{100});
            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->order_count, uint32_t{1});
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{1});
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{10});

            // Leftover 5 (25 requested - 20 eligible) rests as a new bid.
            HYDRA_CHECK(f.book->best_bid().has_value());
            HYDRA_CHECK_EQ(f.book->best_bid().value(), int64_t{101});
            Level *bid_lvl = f.book->best_bid_level();
            HYDRA_CHECK(bid_lvl != nullptr);
            HYDRA_CHECK_EQ(bid_lvl->head_->qty, uint32_t{5});
        }

        void test_cancel_of_partially_filled_resting_order_updates_book_correctly()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));

            const MatchStats n = f.matcher->match(make_order(10, 100, 4, Side::BUY),
                                                [](const FillEvent &) {});
            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{1});

            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{6});
            HYDRA_CHECK_EQ(lvl->total_qty, uint32_t{6});

            HYDRA_CHECK(f.book->cancel_order(1));
            HYDRA_CHECK(!f.book->best_ask().has_value());
            HYDRA_CHECK_EQ(f.book->ask_level_count(), std::size_t{0});
        }

        // Sequential partial fills of the SAME resting order across THREE
        // independent match() calls (not one sweep) -- realistic order
        // flow the canonical 15-case suite's single-match scenarios don't
        // exercise.
        void test_repeated_partial_fills_across_multiple_match_calls()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 30, Side::SELL));

            const MatchStats n1 = f.matcher->match(make_order(10, 100, 10, Side::BUY),
                                                 [](const FillEvent &) {});
            HYDRA_CHECK_EQ(n1.fills_generated, uint32_t{1});
            HYDRA_CHECK(f.book->best_ask().has_value());
            HYDRA_CHECK_EQ(f.book->best_ask_level()->head_->qty, uint32_t{20});

            const MatchStats n2 = f.matcher->match(make_order(11, 100, 15, Side::BUY),
                                                 [](const FillEvent &) {});
            HYDRA_CHECK_EQ(n2.fills_generated, uint32_t{1});
            HYDRA_CHECK(f.book->best_ask().has_value());
            HYDRA_CHECK_EQ(f.book->best_ask_level()->head_->qty, uint32_t{5});

            const MatchStats n3 = f.matcher->match(make_order(12, 100, 5, Side::BUY),
                                                 [](const FillEvent &) {});
            HYDRA_CHECK_EQ(n3.fills_generated, uint32_t{1});
            HYDRA_CHECK(!f.book->best_ask().has_value());
        }

        // Large-scale internal-consistency invariant: after a sweep that
        // partially drains many price levels, every remaining level's
        // total_qty must equal the sum of its FIFO chain's individual
        // order quantities -- catches any accounting drift a small
        // hand-crafted scenario could miss.
        void test_large_scale_multi_level_consistency_invariant()
        {
            Fixture f;

            constexpr int kLevels = 50;
            constexpr int kOrdersPerLevel = 10;
            uint64_t next_id = 1;
            uint64_t total_liquidity = 0;

            for (int level = 0; level < kLevels; ++level)
            {
                const int64_t price = 100 + level;
                for (int i = 0; i < kOrdersPerLevel; ++i)
                {
                    const uint32_t qty = static_cast<uint32_t>((next_id % 20) + 1);
                    HYDRA_CHECK(f.book->add_order(
                                    make_order(next_id, price, qty, Side::SELL)) != nullptr);
                    total_liquidity += qty;
                    ++next_id;
                }
            }
            HYDRA_CHECK_EQ(f.book->ask_level_count(), static_cast<std::size_t>(kLevels));

            // Consume roughly the first third of total liquidity, leaving
            // plenty of untouched levels to check the invariant against.
            const uint32_t sweep_qty = static_cast<uint32_t>(total_liquidity / 3);
            uint64_t total_filled = 0;
            const MatchStats n = f.matcher->match(
                make_order(999'999, 100 + kLevels, sweep_qty, Side::BUY),
                [&](const FillEvent &ev) { total_filled += ev.qty; });
            HYDRA_CHECK(n.fills_generated > 0);
            HYDRA_CHECK_EQ(total_filled, static_cast<uint64_t>(sweep_qty));

            // Walk every remaining level and verify total_qty matches the
            // sum of its resting orders' individual quantities.
            std::size_t levels_checked = 0;
            Level *lvl = f.book->best_ask_level();
            while (lvl != nullptr)
            {
                uint32_t summed = 0;
                std::size_t counted_orders = 0;
                for (Order *o = lvl->head_; o != nullptr; o = o->next_)
                {
                    summed += o->qty;
                    ++counted_orders;
                }
                HYDRA_CHECK_EQ(summed, lvl->total_qty);
                HYDRA_CHECK_EQ(counted_orders, static_cast<std::size_t>(lvl->order_count));
                ++levels_checked;
                lvl = f.book->next_ask_level(lvl->price);
            }
            std::fprintf(stderr, "    checked total_qty consistency across %zu remaining levels\n",
                         levels_checked);
            HYDRA_CHECK(levels_checked > 0);
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_order_pool_exhaustion_during_rest_after_no_liquidity);
    RUN_TEST(test_level_pool_exhaustion_during_rest_at_new_price);
    RUN_TEST(test_ioc_sweeps_multiple_levels_before_discarding_remainder);
    RUN_TEST(test_fok_evaluates_availability_across_multiple_levels);
    RUN_TEST(test_self_trade_prevention_across_multiple_levels_in_sweep);
    RUN_TEST(test_cancel_of_partially_filled_resting_order_updates_book_correctly);
    RUN_TEST(test_repeated_partial_fills_across_multiple_match_calls);
    RUN_TEST(test_large_scale_multi_level_consistency_invariant);

    return report_and_exit_code();
}
