// Matching policies, time-in-force, replace and match-statistics tests.

#include "test_fixtures.hpp"
#include "test_harness.hpp"

namespace hydra::test
{
    namespace
    {

        // Compare FIFO and proportional fills from identical initial books.
        void test_price_time_vs_pro_rata_same_book_state()
        {
            // Price-time: order1 (30) first, then 20 from order2.
            {
                Fixture f;
                (void)f.book->add_order(make_order(1, 100, 30, Side::SELL));
                (void)f.book->add_order(make_order(2, 100, 70, Side::SELL));

                std::vector<FillRecord> fills;
                const MatchStats n =
                    f.matcher->match(make_order(10, 100, 50, Side::BUY), record_fills(fills));

                HYDRA_CHECK_EQ(n.fills_generated, uint32_t{2});
                HYDRA_CHECK_EQ(fills.size(), std::size_t{2});
                HYDRA_CHECK_EQ(fills[0].maker_order_id, uint64_t{1});
                HYDRA_CHECK_EQ(fills[0].qty, uint32_t{30});
                HYDRA_CHECK_EQ(fills[1].maker_order_id, uint64_t{2});
                HYDRA_CHECK_EQ(fills[1].qty, uint32_t{20});

                // order1 is fully consumed; order2 keeps its remaining 50.
                HYDRA_CHECK(f.book->best_ask().has_value());
                Level *lvl = f.book->best_ask_level();
                HYDRA_CHECK(lvl != nullptr);
                HYDRA_CHECK_EQ(lvl->order_count, uint32_t{1});
                HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{2});
                HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{50});
            }

            // Pro-rata: exact shares of 15 and 35.
            {
                Fixture f;
                f.matcher->set_mode(MatchingMode::PRO_RATA);
                (void)f.book->add_order(make_order(1, 100, 30, Side::SELL));
                (void)f.book->add_order(make_order(2, 100, 70, Side::SELL));

                std::vector<FillRecord> fills;
                const MatchStats n =
                    f.matcher->match(make_order(10, 100, 50, Side::BUY), record_fills(fills));

                HYDRA_CHECK_EQ(n.fills_generated, uint32_t{2});
                HYDRA_CHECK_EQ(find_fill_qty(fills, 1), uint32_t{15});
                HYDRA_CHECK_EQ(find_fill_qty(fills, 2), uint32_t{35});
            }
        }

        void test_price_time_strict_fifo_priority()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));
            (void)f.book->add_order(make_order(2, 100, 10, Side::SELL));
            (void)f.book->add_order(make_order(3, 100, 10, Side::SELL));

            std::vector<FillRecord> fills;
            const MatchStats n =
                f.matcher->match(make_order(10, 100, 15, Side::BUY), record_fills(fills));

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{2});
            HYDRA_CHECK_EQ(fills[0].maker_order_id, uint64_t{1});
            HYDRA_CHECK_EQ(fills[0].qty, uint32_t{10});
            HYDRA_CHECK_EQ(fills[1].maker_order_id, uint64_t{2});
            HYDRA_CHECK_EQ(fills[1].qty, uint32_t{5});

            // order3 is untouched.
            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{2});
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{5});
            HYDRA_CHECK_EQ(lvl->head_->next_->order_id, uint64_t{3});
            HYDRA_CHECK_EQ(lvl->head_->next_->qty, uint32_t{10});
        }

        // The residual unit goes to the largest order, which is also the FIFO head.
        void test_pro_rata_residual_goes_to_largest()
        {
            Fixture f;
            f.matcher->set_mode(MatchingMode::PRO_RATA);
            (void)f.book->add_order(make_order(1, 100, 3, Side::SELL)); // largest, FIFO head
            (void)f.book->add_order(make_order(2, 100, 1, Side::SELL));
            (void)f.book->add_order(make_order(3, 100, 1, Side::SELL));

            std::vector<FillRecord> fills;
            const MatchStats n =
                f.matcher->match(make_order(10, 100, 4, Side::BUY), record_fills(fills));

            // Floor shares are 2/0/0; residuals go to order1, then order2.
            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{2});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 1), uint32_t{3});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 2), uint32_t{1});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 3), uint32_t{0});

            uint32_t total = 0;
            for (const auto &fr : fills)
            {
                total += fr.qty;
            }
            HYDRA_CHECK_EQ(total, uint32_t{4});

            // order3 still rests untouched.
            HYDRA_CHECK(f.book->best_ask().has_value());
            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->order_count, uint32_t{1});
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{3});
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{1});
        }

        // Price improvement must execute at the resting maker price.
        void test_price_improvement()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 5, Side::SELL));

            std::vector<FillRecord> fills;
            const MatchStats n =
                f.matcher->match(make_order(10, 105, 5, Side::BUY), record_fills(fills));

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{1});
            HYDRA_CHECK_EQ(fills.size(), std::size_t{1});
            HYDRA_CHECK_EQ(fills[0].price, int64_t{100});
        }

        // An incoming GTC order rests when the book is empty.
        void test_empty_book()
        {
            Fixture f;
            HYDRA_CHECK(!f.book->best_bid().has_value());
            HYDRA_CHECK(!f.book->best_ask().has_value());

            bool handler_called = false;
            const MatchStats n =
                f.matcher->match(make_order(1, 100, 10, Side::BUY),
                                 [&](const FillEvent &) { handler_called = true; });

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{0});
            HYDRA_CHECK(!handler_called);
            HYDRA_CHECK(f.book->best_bid().has_value());
            HYDRA_CHECK_EQ(f.book->best_bid().value(), int64_t{100});
        }

        void test_ioc_leaves_no_resting_remainder()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 3, Side::SELL));

            const MatchStats n = f.matcher->match(
                make_order(10, 100, 10, Side::BUY, TimeInForce::IOC), [](const FillEvent &) {});

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{1});
            HYDRA_CHECK(!f.book->best_bid().has_value());
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{0});
            // The resting ask is fully consumed.
            HYDRA_CHECK(!f.book->best_ask().has_value());
        }

        void test_fok_fills_completely_when_liquidity_sufficient()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));

            bool handler_called = false;
            const MatchStats n =
                f.matcher->match(make_order(10, 100, 10, Side::BUY, TimeInForce::FOK),
                                 [&](const FillEvent &) { handler_called = true; });

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{1});
            HYDRA_CHECK(handler_called);
            HYDRA_CHECK(!f.book->best_ask().has_value());
            HYDRA_CHECK(!f.book->best_bid().has_value());
        }

        void test_fok_mutates_nothing_when_liquidity_insufficient()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 3, Side::SELL));

            bool handler_called = false;
            const MatchStats n =
                f.matcher->match(make_order(10, 100, 10, Side::BUY, TimeInForce::FOK),
                                 [&](const FillEvent &) { handler_called = true; });

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{0});
            HYDRA_CHECK(!handler_called);

            // The book is unchanged and no remainder rests.
            HYDRA_CHECK(f.book->best_ask().has_value());
            HYDRA_CHECK_EQ(f.book->best_ask().value(), int64_t{100});
            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->order_count, uint32_t{1});
            HYDRA_CHECK_EQ(lvl->total_qty, uint32_t{3});
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{3});
            HYDRA_CHECK(!f.book->best_bid().has_value());
        }

        // One Matcher switches between price-time and pro-rata at runtime.
        void test_runtime_mode_toggle_within_single_matcher()
        {
            Fixture f;
            HYDRA_CHECK(f.matcher->mode() == MatchingMode::PRICE_TIME);

            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));
            (void)f.book->add_order(make_order(2, 100, 10, Side::SELL));
            std::vector<FillRecord> price_time_fills;
            const MatchStats n1 = f.matcher->match(make_order(10, 100, 10, Side::BUY),
                                                   record_fills(price_time_fills));
            HYDRA_CHECK_EQ(n1.fills_generated, uint32_t{1});
            HYDRA_CHECK_EQ(price_time_fills[0].maker_order_id, uint64_t{1});
            HYDRA_CHECK_EQ(price_time_fills[0].qty, uint32_t{10});
            HYDRA_CHECK(f.book->cancel_order(2)); // clear the leftover ask

            f.matcher->set_mode(MatchingMode::PRO_RATA);
            HYDRA_CHECK(f.matcher->mode() == MatchingMode::PRO_RATA);

            (void)f.book->add_order(make_order(3, 200, 10, Side::SELL));
            (void)f.book->add_order(make_order(4, 200, 10, Side::SELL));
            std::vector<FillRecord> pro_rata_fills;
            const MatchStats n2 =
                f.matcher->match(make_order(11, 200, 10, Side::BUY), record_fills(pro_rata_fills));
            HYDRA_CHECK_EQ(n2.fills_generated, uint32_t{2});
            HYDRA_CHECK_EQ(find_fill_qty(pro_rata_fills, 3), uint32_t{5});
            HYDRA_CHECK_EQ(find_fill_qty(pro_rata_fills, 4), uint32_t{5});
        }

        // Pro-rata sweeps every crossing level, so the book is never left crossed.
        void test_pro_rata_sweeps_multiple_levels_no_crossed_book()
        {
            Fixture f;
            f.matcher->set_mode(MatchingMode::PRO_RATA);
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));
            (void)f.book->add_order(make_order(2, 101, 100, Side::SELL));

            std::vector<FillRecord> fills;
            const MatchStats n =
                f.matcher->match(make_order(10, 101, 50, Side::BUY), record_fills(fills));

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{2});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 1), uint32_t{10});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 2), uint32_t{40});

            // Fully filled: no bid rests and the book is not crossed.
            HYDRA_CHECK(!f.book->best_bid().has_value());
            HYDRA_CHECK(f.book->best_ask().has_value());
            HYDRA_CHECK_EQ(f.book->best_ask().value(), int64_t{101});
        }

        // A sole resting order that is both FIFO head and largest is fully consumed.
        void test_pro_rata_sole_order_fully_filled_no_crash()
        {
            Fixture f;
            f.matcher->set_mode(MatchingMode::PRO_RATA);
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));

            std::vector<FillRecord> fills;
            const MatchStats n =
                f.matcher->match(make_order(10, 100, 10, Side::BUY), record_fills(fills));

            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{1});
            HYDRA_CHECK_EQ(fills[0].qty, uint32_t{10});
            HYDRA_CHECK(!f.book->best_ask().has_value());
        }

        void test_self_trade_prevention_in_pro_rata()
        {
            Fixture f;
            f.matcher->set_mode(MatchingMode::PRO_RATA);
            (void)f.book->add_order(make_order(1, 100, 5, Side::SELL, TimeInForce::GTC, 7));
            (void)f.book->add_order(make_order(2, 100, 5, Side::SELL, TimeInForce::GTC, 9));

            std::vector<FillRecord> fills;
            const MatchStats n = f.matcher->match(
                make_order(10, 100, 5, Side::BUY, TimeInForce::GTC, 7), record_fills(fills));

            // The self-trade order is excluded; order2 takes the full fill.
            HYDRA_CHECK_EQ(n.fills_generated, uint32_t{1});
            HYDRA_CHECK_EQ(fills[0].maker_order_id, uint64_t{2});
            HYDRA_CHECK_EQ(fills[0].qty, uint32_t{5});

            HYDRA_CHECK(f.book->best_ask().has_value());
            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->order_count, uint32_t{1});
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{1});
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{5});
        }

        // A same-price quantity decrease preserves FIFO position.
        void test_replace_preserves_priority_same_price_qty_decrease()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));
            (void)f.book->add_order(make_order(2, 100, 10, Side::SELL));

            const MatchStats stats = f.matcher->replace(1, 100, 4, 0, [](const FillEvent &) {});
            HYDRA_CHECK_EQ(stats.fills_generated, uint32_t{0});

            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->order_count, uint32_t{2});
            // order1 keeps FIFO head with its reduced qty.
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{1});
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{4});
            HYDRA_CHECK_EQ(lvl->total_qty, uint32_t{14});
        }

        // A price change loses priority and places order1 behind order2.
        void test_replace_loses_priority_on_price_change()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));
            (void)f.book->add_order(make_order(2, 101, 10, Side::SELL));

            const MatchStats stats = f.matcher->replace(1, 101, 10, 0, [](const FillEvent &) {});
            HYDRA_CHECK_EQ(stats.fills_generated, uint32_t{0});

            // The old level is gone.
            HYDRA_CHECK_EQ(f.book->ask_level_count(), std::size_t{1});
            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->price, int64_t{101});
            HYDRA_CHECK_EQ(lvl->order_count, uint32_t{2});
            // order1 moves behind order2.
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{2});
            HYDRA_CHECK_EQ(lvl->head_->next_->order_id, uint64_t{1});
        }

        // A same-price quantity increase loses priority.
        void test_replace_loses_priority_on_qty_increase()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));
            (void)f.book->add_order(make_order(2, 100, 10, Side::SELL));

            const MatchStats stats = f.matcher->replace(1, 100, 20, 0, [](const FillEvent &) {});
            HYDRA_CHECK_EQ(stats.fills_generated, uint32_t{0});

            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->order_count, uint32_t{2});
            // order1 moves behind order2.
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{2});
            HYDRA_CHECK_EQ(lvl->head_->next_->order_id, uint64_t{1});
            HYDRA_CHECK_EQ(lvl->head_->next_->qty, uint32_t{20});
        }

        // A replacement at a crossing price must match immediately.
        void test_replace_that_crosses_book()
        {
            Fixture f;
            Order replaced = make_order(1, 105, 10, Side::SELL); // to be replaced
            replaced.timestamp_ns = 123;
            (void)f.book->add_order(replaced);
            (void)f.book->add_order(make_order(2, 100, 10, Side::BUY)); // resting bid

            std::vector<FillRecord> fills;
            uint64_t fill_timestamp_ns = 0;
            // Repricing to 100 crosses the resting bid.
            const MatchStats stats =
                f.matcher->replace(1, 100, 10, 456,
                                   [&](const FillEvent &fill)
                                   {
                                       fills.push_back({fill.maker_order_id, fill.price, fill.qty});
                                       fill_timestamp_ns = fill.timestamp_ns;
                                   });

            HYDRA_CHECK_EQ(stats.fills_generated, uint32_t{1});
            HYDRA_CHECK_EQ(fills.size(), std::size_t{1});
            HYDRA_CHECK_EQ(fills[0].maker_order_id, uint64_t{2});
            HYDRA_CHECK_EQ(fills[0].qty, uint32_t{10});
            HYDRA_CHECK_EQ(fill_timestamp_ns, uint64_t{456});
            HYDRA_CHECK(!f.book->best_bid().has_value());
            HYDRA_CHECK(!f.book->best_ask().has_value());
        }

        void test_replace_nonexistent_order_is_noop()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));

            const MatchStats stats = f.matcher->replace(999, 100, 5, 0, [](const FillEvent &) {});
            HYDRA_CHECK_EQ(stats.fills_generated, uint32_t{0});

            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->order_count, uint32_t{1});
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{1});
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{10});
        }

        // A two-level sweep counts skipped self-trades as examined but ineligible.
        void test_match_stats_levels_and_examined_counts_price_time()
        {
            Fixture f;
            (void)f.book->add_order(
                make_order(1, 100, 5, Side::SELL, TimeInForce::GTC, 7)); // self-trade
            (void)f.book->add_order(make_order(2, 100, 5, Side::SELL, TimeInForce::GTC, 9));
            (void)f.book->add_order(make_order(3, 101, 20, Side::SELL, TimeInForce::GTC, 9));

            const MatchStats stats = f.matcher->match(
                make_order(10, 101, 15, Side::BUY, TimeInForce::GTC, 7), [](const FillEvent &) {});

            HYDRA_CHECK_EQ(stats.levels_consumed, uint32_t{2});
            HYDRA_CHECK_EQ(stats.self_trade_skips, uint32_t{1});
            HYDRA_CHECK_EQ(stats.resting_orders_examined, uint32_t{3});
            HYDRA_CHECK_EQ(stats.eligible_orders_examined, uint32_t{2});
            HYDRA_CHECK_EQ(stats.fills_generated, uint32_t{2});
            HYDRA_CHECK_EQ(stats.remaining_qty, uint32_t{0});
        }

        // remaining_qty reports the unfilled part of a partial fill.
        void test_match_stats_remaining_qty_on_partial_fill()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 5, Side::SELL));

            const MatchStats stats = f.matcher->match(
                make_order(10, 100, 12, Side::BUY, TimeInForce::IOC), [](const FillEvent &) {});

            HYDRA_CHECK_EQ(stats.fills_generated, uint32_t{1});
            HYDRA_CHECK_EQ(stats.remaining_qty, uint32_t{7});
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_price_time_vs_pro_rata_same_book_state);
    RUN_TEST(test_price_time_strict_fifo_priority);
    RUN_TEST(test_pro_rata_residual_goes_to_largest);
    RUN_TEST(test_price_improvement);
    RUN_TEST(test_empty_book);
    RUN_TEST(test_ioc_leaves_no_resting_remainder);
    RUN_TEST(test_fok_fills_completely_when_liquidity_sufficient);
    RUN_TEST(test_fok_mutates_nothing_when_liquidity_insufficient);
    RUN_TEST(test_runtime_mode_toggle_within_single_matcher);
    RUN_TEST(test_pro_rata_sweeps_multiple_levels_no_crossed_book);
    RUN_TEST(test_pro_rata_sole_order_fully_filled_no_crash);
    RUN_TEST(test_self_trade_prevention_in_pro_rata);
    RUN_TEST(test_replace_preserves_priority_same_price_qty_decrease);
    RUN_TEST(test_replace_loses_priority_on_price_change);
    RUN_TEST(test_replace_loses_priority_on_qty_increase);
    RUN_TEST(test_replace_that_crosses_book);
    RUN_TEST(test_replace_nonexistent_order_is_noop);
    RUN_TEST(test_match_stats_levels_and_examined_counts_price_time);
    RUN_TEST(test_match_stats_remaining_qty_on_partial_fill);

    return report_and_exit_code();
}
