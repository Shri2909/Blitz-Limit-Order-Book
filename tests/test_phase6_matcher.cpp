//===----------------------------------------------------------------------===
// tests/test_phase6_matcher.cpp
//
// Phase 6 exit condition (see HYDRA-LOB-Roadmap.md):
//   "price-time and pro-rata produce correct fills on the same book state;
//   IOC/FOK behave per spec; runtime mode toggle verified."
//
// WHY this binary is itself built with -fsanitize=address,undefined (see
// CMakeLists.txt's blitz_lob_test_phase6_matcher target): the pro-rata path
// in this exact file previously had a real use-after-free (the "largest"
// resting order could be unlinked and freed back into its pool mid-
// traversal, before the FIFO walk had finished reading its intrusive
// next_ pointer) and a crossed-book bug (pro-rata only ever swept the best
// price level). Both are fixed, but this phase's own test file is where a
// regression of either would first show up, so it runs under ASan as a
// durable guard rather than trusting "it compiled and the numbers matched."
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
            std::unique_ptr<ObjectPool<FillEvent, FILL_EVENT_POOL_SIZE>> fill_pool =
                std::make_unique<ObjectPool<FillEvent, FILL_EVENT_POOL_SIZE>>();
            std::unique_ptr<OrderBook> book =
                std::make_unique<OrderBook>(*order_pool, *level_pool);
            std::unique_ptr<Matcher> matcher = std::make_unique<Matcher>(
                *book, *fill_pool, MatchingMode::PRICE_TIME);
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

        // Roadmap's core Phase 6 validation: the SAME starting book state,
        // matched once under PRICE_TIME and once under PRO_RATA, must
        // produce individually-correct but mode-appropriate fill
        // distributions -- strict FIFO priority vs. proportional split.
        void test_price_time_vs_pro_rata_same_book_state()
        {
            // PRICE_TIME: order1 (arrived first) must be fully consumed
            // before order2 is touched at all -- but since incoming (50)
            // exceeds order1's qty (30), the sweep must still spill the
            // leftover 20 into order2 in the same match() call (price-time
            // sweeps the whole crossing level, not just its first order).
            {
                Fixture f;
                (void)f.book->add_order(make_order(1, 100, 30, Side::SELL));
                (void)f.book->add_order(make_order(2, 100, 70, Side::SELL));

                std::vector<FillRecord> fills;
                const uint32_t n = f.matcher->match(
                    make_order(10, 100, 50, Side::BUY), [&](const FillEvent &ev)
                    { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });

                HYDRA_CHECK_EQ(n, uint32_t{2});
                HYDRA_CHECK_EQ(fills.size(), std::size_t{2});
                HYDRA_CHECK_EQ(fills[0].maker_order_id, uint64_t{1});
                HYDRA_CHECK_EQ(fills[0].qty, uint32_t{30});
                HYDRA_CHECK_EQ(fills[1].maker_order_id, uint64_t{2});
                HYDRA_CHECK_EQ(fills[1].qty, uint32_t{20});

                // order1 fully consumed (removed); order2 left resting with
                // its remaining 50 -- strict FIFO priority, not a split.
                HYDRA_CHECK(f.book->best_ask().has_value());
                Level *lvl = f.book->best_ask_level();
                HYDRA_CHECK(lvl != nullptr);
                HYDRA_CHECK_EQ(lvl->order_count, uint32_t{1});
                HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{2});
                HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{50});
            }

            // PRO_RATA: the identical starting book (order1=30, order2=70,
            // incoming=50) must instead split proportionally to each
            // resting order's share of the level (30% / 70% of 50 = 15/35,
            // dividing evenly with zero residual for this pair of
            // quantities, so the result is exact, not just "close").
            {
                Fixture f;
                f.matcher->set_mode(MatchingMode::PRO_RATA);
                (void)f.book->add_order(make_order(1, 100, 30, Side::SELL));
                (void)f.book->add_order(make_order(2, 100, 70, Side::SELL));

                std::vector<FillRecord> fills;
                const uint32_t n = f.matcher->match(
                    make_order(10, 100, 50, Side::BUY), [&](const FillEvent &ev)
                    { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });

                HYDRA_CHECK_EQ(n, uint32_t{2});
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
            const uint32_t n = f.matcher->match(
                make_order(10, 100, 15, Side::BUY), [&](const FillEvent &ev)
                { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });

            HYDRA_CHECK_EQ(n, uint32_t{2});
            HYDRA_CHECK_EQ(fills[0].maker_order_id, uint64_t{1});
            HYDRA_CHECK_EQ(fills[0].qty, uint32_t{10});
            HYDRA_CHECK_EQ(fills[1].maker_order_id, uint64_t{2});
            HYDRA_CHECK_EQ(fills[1].qty, uint32_t{5});

            // order3 must be completely untouched.
            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{2});
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{5});
            HYDRA_CHECK_EQ(lvl->head_->next_->order_id, uint64_t{3});
            HYDRA_CHECK_EQ(lvl->head_->next_->qty, uint32_t{10});
        }

        // Exercises the "residual rule" explicitly with a quantity split
        // that does NOT divide evenly, and doubles as the exact scenario
        // that used to trigger the pro-rata use-after-free: order1 is both
        // the FIFO head *and* the largest resting order, and gets fully
        // consumed by its deferred, dedicated fill.
        void test_pro_rata_residual_goes_to_largest()
        {
            Fixture f;
            f.matcher->set_mode(MatchingMode::PRO_RATA);
            (void)f.book->add_order(make_order(1, 100, 3, Side::SELL)); // largest, FIFO head
            (void)f.book->add_order(make_order(2, 100, 1, Side::SELL));
            (void)f.book->add_order(make_order(3, 100, 1, Side::SELL));

            std::vector<FillRecord> fills;
            const uint32_t n = f.matcher->match(
                make_order(10, 100, 4, Side::BUY), [&](const FillEvent &ev)
                { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });

            // floor shares: order1=floor(4*3/5)=2, order2=floor(4*1/5)=0,
            // order3=floor(4*1/5)=0 -- total_floor=2, residual=2. order1
            // (largest) absorbs residual up to its headroom (3-2=1), taking
            // 1 of the 2 residual units -> total_share=3 (fully filled).
            // Remaining residual=1 goes to the next order in FIFO order
            // with headroom (order2: floor=0, headroom=1) -> total_share=1
            // (fully filled). order3 gets floor=0 and no residual left ->
            // no fill at all.
            HYDRA_CHECK_EQ(n, uint32_t{2});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 1), uint32_t{3});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 2), uint32_t{1});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 3), uint32_t{0});

            uint32_t total = 0;
            for (const auto &fr : fills)
            {
                total += fr.qty;
            }
            HYDRA_CHECK_EQ(total, uint32_t{4});

            // order3 must still be resting, untouched.
            HYDRA_CHECK(f.book->best_ask().has_value());
            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->order_count, uint32_t{1});
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{3});
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{1});
        }

        void test_ioc_leaves_no_resting_remainder()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 3, Side::SELL));

            const uint32_t n = f.matcher->match(
                make_order(10, 100, 10, Side::BUY, TimeInForce::IOC), [](const FillEvent &) {});

            HYDRA_CHECK_EQ(n, uint32_t{1});
            HYDRA_CHECK(!f.book->best_bid().has_value());
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{0});
            // The ask side's order1 is fully consumed too.
            HYDRA_CHECK(!f.book->best_ask().has_value());
        }

        void test_fok_fills_completely_when_liquidity_sufficient()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));

            bool handler_called = false;
            const uint32_t n = f.matcher->match(
                make_order(10, 100, 10, Side::BUY, TimeInForce::FOK),
                [&](const FillEvent &) { handler_called = true; });

            HYDRA_CHECK_EQ(n, uint32_t{1});
            HYDRA_CHECK(handler_called);
            HYDRA_CHECK(!f.book->best_ask().has_value());
            HYDRA_CHECK(!f.book->best_bid().has_value());
        }

        void test_fok_mutates_nothing_when_liquidity_insufficient()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 3, Side::SELL));

            bool handler_called = false;
            const uint32_t n = f.matcher->match(
                make_order(10, 100, 10, Side::BUY, TimeInForce::FOK),
                [&](const FillEvent &) { handler_called = true; });

            HYDRA_CHECK_EQ(n, uint32_t{0});
            HYDRA_CHECK(!handler_called);

            // Book must be byte-for-byte unchanged: same resting qty, same
            // level state, and no bid rested either (a failed FOK never
            // rests any remainder).
            HYDRA_CHECK(f.book->best_ask().has_value());
            HYDRA_CHECK_EQ(f.book->best_ask().value(), int64_t{100});
            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->order_count, uint32_t{1});
            HYDRA_CHECK_EQ(lvl->total_qty, uint32_t{3});
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{3});
            HYDRA_CHECK(!f.book->best_bid().has_value());
        }

        // Roadmap: "runtime set_mode() toggle verified by matching once in
        // each mode within a single test run" -- one Matcher instance, one
        // book, matched once per mode. order2 is deliberately left resting
        // (untouched) by the first match's exact-qty fill of order1 -- it
        // is explicitly cancelled below before the second scenario, so the
        // PRO_RATA match isn't accidentally satisfied by that leftover
        // (lower, therefore better-priced) ask level instead of the new
        // orders it's meant to exercise.
        void test_runtime_mode_toggle_within_single_matcher()
        {
            Fixture f;
            HYDRA_CHECK(f.matcher->mode() == MatchingMode::PRICE_TIME);

            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));
            (void)f.book->add_order(make_order(2, 100, 10, Side::SELL));
            std::vector<FillRecord> price_time_fills;
            const uint32_t n1 = f.matcher->match(
                make_order(10, 100, 10, Side::BUY), [&](const FillEvent &ev)
                { price_time_fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });
            HYDRA_CHECK_EQ(n1, uint32_t{1});
            HYDRA_CHECK_EQ(price_time_fills[0].maker_order_id, uint64_t{1});
            HYDRA_CHECK_EQ(price_time_fills[0].qty, uint32_t{10});
            HYDRA_CHECK(f.book->cancel_order(2)); // clear the leftover resting order

            f.matcher->set_mode(MatchingMode::PRO_RATA);
            HYDRA_CHECK(f.matcher->mode() == MatchingMode::PRO_RATA);

            (void)f.book->add_order(make_order(3, 200, 10, Side::SELL));
            (void)f.book->add_order(make_order(4, 200, 10, Side::SELL));
            std::vector<FillRecord> pro_rata_fills;
            const uint32_t n2 = f.matcher->match(
                make_order(11, 200, 10, Side::BUY), [&](const FillEvent &ev)
                { pro_rata_fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });
            HYDRA_CHECK_EQ(n2, uint32_t{2});
            HYDRA_CHECK_EQ(find_fill_qty(pro_rata_fills, 3), uint32_t{5});
            HYDRA_CHECK_EQ(find_fill_qty(pro_rata_fills, 4), uint32_t{5});
        }

        // Regression test for the crossed-book bug: pro-rata used to match
        // only the single best price level, so a marketable order larger
        // than that level's depth would leave the leftover GTC quantity
        // resting at a price that still crossed the opposite side.
        void test_pro_rata_sweeps_multiple_levels_no_crossed_book()
        {
            Fixture f;
            f.matcher->set_mode(MatchingMode::PRO_RATA);
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));
            (void)f.book->add_order(make_order(2, 101, 100, Side::SELL));

            std::vector<FillRecord> fills;
            const uint32_t n = f.matcher->match(
                make_order(10, 101, 50, Side::BUY), [&](const FillEvent &ev)
                { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });

            HYDRA_CHECK_EQ(n, uint32_t{2});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 1), uint32_t{10});
            HYDRA_CHECK_EQ(find_fill_qty(fills, 2), uint32_t{40});

            const auto bb = f.book->best_bid();
            const auto ba = f.book->best_ask();
            // Fully filled (50 of 50) -- nothing should rest on the bid
            // side at all.
            HYDRA_CHECK(!bb.has_value());
            HYDRA_CHECK(ba.has_value());
            HYDRA_CHECK_EQ(ba.value(), int64_t{101});
            // The one invariant that must never be violated:
            if (bb.has_value() && ba.has_value())
            {
                HYDRA_CHECK(bb.value() < ba.value());
            }
        }

        // Minimal, direct regression test for the pro-rata use-after-free:
        // a single resting order that is simultaneously the FIFO head and
        // the "largest" order, fully consumed by the incoming quantity.
        void test_pro_rata_sole_order_fully_filled_no_crash()
        {
            Fixture f;
            f.matcher->set_mode(MatchingMode::PRO_RATA);
            (void)f.book->add_order(make_order(1, 100, 10, Side::SELL));

            std::vector<FillRecord> fills;
            const uint32_t n = f.matcher->match(
                make_order(10, 100, 10, Side::BUY), [&](const FillEvent &ev)
                { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });

            HYDRA_CHECK_EQ(n, uint32_t{1});
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
            const uint32_t n = f.matcher->match(
                make_order(10, 100, 5, Side::BUY, TimeInForce::GTC, 7),
                [&](const FillEvent &ev)
                { fills.push_back({ev.maker_order_id, ev.price, ev.qty}); });

            // order1 shares incoming's client_id (7) -- must be skipped
            // entirely, leaving order2 as the sole eligible participant and
            // therefore receiving the ENTIRE incoming quantity, not a
            // proportional split against order1's (excluded) quantity.
            HYDRA_CHECK_EQ(n, uint32_t{1});
            HYDRA_CHECK_EQ(fills[0].maker_order_id, uint64_t{2});
            HYDRA_CHECK_EQ(fills[0].qty, uint32_t{5});

            HYDRA_CHECK(f.book->best_ask().has_value());
            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->order_count, uint32_t{1});
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{1});
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{5});
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_price_time_vs_pro_rata_same_book_state);
    RUN_TEST(test_price_time_strict_fifo_priority);
    RUN_TEST(test_pro_rata_residual_goes_to_largest);
    RUN_TEST(test_ioc_leaves_no_resting_remainder);
    RUN_TEST(test_fok_fills_completely_when_liquidity_sufficient);
    RUN_TEST(test_fok_mutates_nothing_when_liquidity_insufficient);
    RUN_TEST(test_runtime_mode_toggle_within_single_matcher);
    RUN_TEST(test_pro_rata_sweeps_multiple_levels_no_crossed_book);
    RUN_TEST(test_pro_rata_sole_order_fully_filled_no_crash);
    RUN_TEST(test_self_trade_prevention_in_pro_rata);

    return report_and_exit_code();
}
