// Matcher edge cases: pool exhaustion, multi-level sweeps, self-trades,
// partial fills and book consistency.

#include "test_fixtures.hpp"
#include "test_harness.hpp"

#include <cstdio>

namespace hydra::test
{
    namespace
    {

        // With the order pool exhausted, a GTC order cannot rest.
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

            const MatchStats n =
                f.matcher->match(make_order(10, 100, 10, Side::BUY), [](const FillEvent &) {});

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{0});
            HYDRA_CHECK(!f.book->best_bid().has_value());
        }

        // With the level pool exhausted, resting at a new price fails cleanly.
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

            const MatchStats n =
                f.matcher->match(make_order(10, 100, 10, Side::BUY), [](const FillEvent &) {});

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{0});
            HYDRA_CHECK(!f.book->best_bid().has_value());
            // The order acquired for the failed rest is released, not leaked.
            HYDRA_CHECK_EQ(f.order_pool->exhaustion_count(), uint64_t{0});
        }

        void test_ioc_sweeps_multiple_levels_before_discarding_remainder()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));
            (void)f.book->add_order(make_order(2, 101, 10, Side::SELL));

            std::vector<FillRecord> fills;
            const MatchStats n = f.matcher->match(
                make_order(10, 101, 25, Side::BUY, TimeInForce::IOC), record_fills(fills));

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{2});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 1), uint32_t{10});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 2), uint32_t{10});
            // Both levels drain; the unfilled 5 units are discarded, not rested.
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
                    make_order(10, 101, 20, Side::BUY, TimeInForce::FOK), record_fills(fills));

                HYDRA_CHECK_EQ(n.fills_generated, uint32_t{2});
                HYDRA_CHECK_EQ(find_fill_qty(fills, 1), uint32_t{10});
                HYDRA_CHECK_EQ(find_fill_qty(fills, 2), uint32_t{10});
                HYDRA_CHECK(f.book->best_ask().has_value());
                Level *lvl = f.book->best_ask_level();
                HYDRA_CHECK(lvl != nullptr);
                HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{2});
                HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{5});
            }

            // Failure case: 30 requested, 25 available; nothing may change.
            {
                Fixture f;
                (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));
                (void)f.book->add_order(make_order(2, 101, 15, Side::SELL));

                bool handler_called = false;
                const MatchStats n =
                    f.matcher->match(make_order(10, 101, 30, Side::BUY, TimeInForce::FOK),
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

        // Skipping a self-trade still allows matching within and across levels.
        void test_self_trade_prevention_across_multiple_levels_in_sweep()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL, TimeInForce::GTC, 7));
            (void)f.book->add_order(make_order(2, 100, 10, Side::SELL, TimeInForce::GTC, 9));
            (void)f.book->add_order(make_order(3, 101, 10, Side::SELL, TimeInForce::GTC, 9));

            std::vector<FillRecord> fills;
            const MatchStats n = f.matcher->match(
                make_order(10, 101, 25, Side::BUY, TimeInForce::GTC, 7), record_fills(fills));

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{2});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 2), uint32_t{10});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 3), uint32_t{10});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 1), uint32_t{0});

            // The self-trade order still rests untouched at 100.
            HYDRA_CHECK(f.book->best_ask().has_value());
            HYDRA_CHECK_EQ(f.book->best_ask().value(), int64_t{100});
            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->order_count, uint32_t{1});
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{1});
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{10});

            // The remainder is discarded rather than resting against the skipped ask.
            HYDRA_CHECK(!f.book->best_bid().has_value());
            HYDRA_CHECK_EQ(n.remaining_qty, uint32_t{5});
        }

        void test_cancel_of_partially_filled_resting_order_updates_book_correctly()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));

            const MatchStats n =
                f.matcher->match(make_order(10, 100, 4, Side::BUY), [](const FillEvent &) {});
            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{1});

            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{6});
            HYDRA_CHECK_EQ(lvl->total_qty, uint32_t{6});

            HYDRA_CHECK(f.book->cancel_order(1));
            HYDRA_CHECK(!f.book->best_ask().has_value());
            HYDRA_CHECK_EQ(f.book->ask_level_count(), std::size_t{0});
        }

        // One resting order is partially filled across three match calls.
        void test_repeated_partial_fills_across_multiple_match_calls()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 30, Side::SELL));

            const MatchStats n1 =
                f.matcher->match(make_order(10, 100, 10, Side::BUY), [](const FillEvent &) {});
            HYDRA_CHECK_EQ(n1.fills_generated, uint32_t{1});
            HYDRA_CHECK(f.book->best_ask().has_value());
            HYDRA_CHECK_EQ(f.book->best_ask_level()->head_->qty, uint32_t{20});

            const MatchStats n2 =
                f.matcher->match(make_order(11, 100, 15, Side::BUY), [](const FillEvent &) {});
            HYDRA_CHECK_EQ(n2.fills_generated, uint32_t{1});
            HYDRA_CHECK(f.book->best_ask().has_value());
            HYDRA_CHECK_EQ(f.book->best_ask_level()->head_->qty, uint32_t{5});

            const MatchStats n3 =
                f.matcher->match(make_order(12, 100, 5, Side::BUY), [](const FillEvent &) {});
            HYDRA_CHECK_EQ(n3.fills_generated, uint32_t{1});
            HYDRA_CHECK(!f.book->best_ask().has_value());
        }

        // Each surviving level's total_qty equals the sum of its orders' quantities.
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
                    HYDRA_CHECK(f.book->add_order(make_order(next_id, price, qty, Side::SELL)) !=
                                nullptr);
                    total_liquidity += qty;
                    ++next_id;
                }
            }
            HYDRA_CHECK_EQ(f.book->ask_level_count(), static_cast<std::size_t>(kLevels));

            // Consume about a third of the liquidity.
            const uint32_t sweep_qty = static_cast<uint32_t>(total_liquidity / 3);
            uint64_t total_filled = 0;
            const MatchStats n =
                f.matcher->match(make_order(999'999, 100 + kLevels, sweep_qty, Side::BUY),
                                 [&](const FillEvent &ev) { total_filled += ev.qty; });
            HYDRA_CHECK(n.fills_generated > 0);
            HYDRA_CHECK_EQ(total_filled, static_cast<uint64_t>(sweep_qty));

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
