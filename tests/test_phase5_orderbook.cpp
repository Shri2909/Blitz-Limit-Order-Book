//===----------------------------------------------------------------------===
// tests/test_phase5_orderbook.cpp
//
// Phase 5 exit condition (see HYDRA-LOB-Roadmap.md):
//   "add/cancel/best_bid/best_ask correct against hand-written cases; O(1)
//   cancel verified by inspection."
//
// The "by inspection" claim is backed here by more than a comment: the last
// test in this file is a micro-benchmark that cancels an order at the head
// vs. the tail of a long resting FIFO chain and confirms the timings are
// statistically indistinguishable -- if cancel_order() ever regressed to a
// linear scan of the FIFO to find its target (instead of going straight to
// it via the order_index_ handle), that ratio would blow out toward the
// FIFO depth, not stay near 1:1.
//===----------------------------------------------------------------------===

#include "hydra/order_book.hpp"

#include "test_harness.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

namespace hydra::test
{
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

        struct Fixture
        {
            std::unique_ptr<ObjectPool<Order, ORDER_POOL_SIZE>> order_pool =
                std::make_unique<ObjectPool<Order, ORDER_POOL_SIZE>>();
            std::unique_ptr<ObjectPool<Level, LEVEL_POOL_SIZE>> level_pool =
                std::make_unique<ObjectPool<Level, LEVEL_POOL_SIZE>>();
            std::unique_ptr<OrderBook> book =
                std::make_unique<OrderBook>(*order_pool, *level_pool);
        };

        void test_add_multiple_orders_multiple_price_levels()
        {
            Fixture f;

            HYDRA_CHECK(f.book->add_order(make_order(1, 100, 10, Side::BUY)) != nullptr);
            HYDRA_CHECK(f.book->add_order(make_order(2, 99, 5, Side::BUY)) != nullptr);
            HYDRA_CHECK(f.book->add_order(make_order(3, 101, 5, Side::BUY)) != nullptr);
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{3});
            HYDRA_CHECK_EQ(f.book->best_bid().value(), int64_t{101});

            HYDRA_CHECK(f.book->add_order(make_order(4, 105, 5, Side::SELL)) != nullptr);
            HYDRA_CHECK(f.book->add_order(make_order(5, 103, 5, Side::SELL)) != nullptr);
            HYDRA_CHECK_EQ(f.book->ask_level_count(), std::size_t{2});
            HYDRA_CHECK_EQ(f.book->best_ask().value(), int64_t{103});

            // A second order at an already-existing price must NOT create a
            // new level.
            HYDRA_CHECK(f.book->add_order(make_order(6, 101, 3, Side::BUY)) != nullptr);
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{3});
        }

        void test_best_bid_ask_after_cancel_empties_level()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 101, 10, Side::BUY));
            (void)f.book->add_order(make_order(2, 100, 10, Side::BUY));
            HYDRA_CHECK_EQ(f.book->best_bid().value(), int64_t{101});

            HYDRA_CHECK(f.book->cancel_order(1));
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{1});
            HYDRA_CHECK_EQ(f.book->best_bid().value(), int64_t{100});

            HYDRA_CHECK(f.book->cancel_order(2));
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{0});
            HYDRA_CHECK(!f.book->best_bid().has_value());
        }

        void test_fifo_ordering_within_level()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 1, Side::SELL));
            (void)f.book->add_order(make_order(2, 100, 1, Side::SELL));
            (void)f.book->add_order(make_order(3, 100, 1, Side::SELL));

            Level *lvl = f.book->best_ask_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->order_count, uint32_t{3});

            // Walk the intrusive FIFO chain head-to-tail and confirm arrival
            // order is preserved exactly.
            Order *cursor = lvl->head_;
            HYDRA_CHECK(cursor != nullptr);
            HYDRA_CHECK_EQ(cursor->order_id, uint64_t{1});
            cursor = cursor->next_;
            HYDRA_CHECK(cursor != nullptr);
            HYDRA_CHECK_EQ(cursor->order_id, uint64_t{2});
            cursor = cursor->next_;
            HYDRA_CHECK(cursor != nullptr);
            HYDRA_CHECK_EQ(cursor->order_id, uint64_t{3});
            cursor = cursor->next_;
            HYDRA_CHECK(cursor == nullptr);
            HYDRA_CHECK_EQ(lvl->tail_->order_id, uint64_t{3});

            // Cancelling the middle order must relink head/tail correctly,
            // not just decrement a counter.
            HYDRA_CHECK(f.book->cancel_order(2));
            HYDRA_CHECK_EQ(lvl->head_->order_id, uint64_t{1});
            HYDRA_CHECK_EQ(lvl->head_->next_->order_id, uint64_t{3});
            HYDRA_CHECK(lvl->head_->next_->next_ == nullptr);
            HYDRA_CHECK_EQ(lvl->tail_->order_id, uint64_t{3});
        }

        void test_cancel_unknown_order_returns_false()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 1, Side::BUY));
            HYDRA_CHECK(!f.book->cancel_order(999));
            // Book state must be untouched by the failed cancel.
            HYDRA_CHECK_EQ(f.book->bid_level_count(), std::size_t{1});
        }

        void test_cancel_twice_second_call_fails()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 1, Side::BUY));
            HYDRA_CHECK(f.book->cancel_order(1));
            HYDRA_CHECK(!f.book->cancel_order(1));
        }

        void test_duplicate_order_id_rejected()
        {
            Fixture f;
            HYDRA_CHECK(f.book->add_order(make_order(1, 100, 5, Side::BUY)) != nullptr);
            HYDRA_CHECK(f.book->add_order(make_order(1, 200, 3, Side::BUY)) == nullptr);
            // Original order must be untouched -- still resting at its
            // original price/qty, not overwritten by the rejected duplicate.
            HYDRA_CHECK_EQ(f.book->best_bid().value(), int64_t{100});
            Level *lvl = f.book->best_bid_level();
            HYDRA_CHECK(lvl != nullptr);
            HYDRA_CHECK_EQ(lvl->head_->qty, uint32_t{5});
        }

        void test_level_pool_exhaustion_handled_cleanly()
        {
            Fixture f;
            for (std::size_t i = 0; i < LEVEL_POOL_SIZE; ++i)
            {
                Order *r = f.book->add_order(
                    make_order(i + 1, static_cast<int64_t>(i), 1, Side::BUY));
                HYDRA_CHECK(r != nullptr);
            }
            HYDRA_CHECK_EQ(f.book->bid_level_count(), LEVEL_POOL_SIZE);

            // A brand-new price needs a brand-new Level -- pool is
            // genuinely exhausted, so this must fail cleanly, not crash.
            Order *overflow = f.book->add_order(
                make_order(999'999, static_cast<int64_t>(LEVEL_POOL_SIZE), 1, Side::BUY));
            HYDRA_CHECK(overflow == nullptr);
            HYDRA_CHECK_EQ(f.book->bid_level_count(), LEVEL_POOL_SIZE);

            // An order at a price that already HAS a level must still
            // succeed -- exhaustion only blocks *new* levels.
            Order *still_works = f.book->add_order(make_order(999'998, 0, 1, Side::BUY));
            HYDRA_CHECK(still_works != nullptr);
        }

        void test_next_level_traversal_visits_in_price_order()
        {
            Fixture f;
            (void)f.book->add_order(make_order(1, 100, 1, Side::BUY));
            (void)f.book->add_order(make_order(2, 102, 1, Side::BUY));
            (void)f.book->add_order(make_order(3, 101, 1, Side::BUY));

            Level *l0 = f.book->best_bid_level();
            HYDRA_CHECK(l0 != nullptr);
            HYDRA_CHECK_EQ(l0->price, int64_t{102});

            Level *l1 = f.book->next_bid_level(l0->price);
            HYDRA_CHECK(l1 != nullptr);
            HYDRA_CHECK_EQ(l1->price, int64_t{101});

            Level *l2 = f.book->next_bid_level(l1->price);
            HYDRA_CHECK(l2 != nullptr);
            HYDRA_CHECK_EQ(l2->price, int64_t{100});

            HYDRA_CHECK(f.book->next_bid_level(l2->price) == nullptr);
        }

        // O(1) cancel evidence: cancel an order at the head vs. the tail of
        // a long FIFO and confirm the average wall-clock cost doesn't scale
        // with position -- see file header comment for the rationale.
        void test_cancel_is_position_independent_micro_benchmark()
        {
            Fixture f;

            constexpr int kLevelDepth = 5000;
            constexpr int kTrials = 100;

            uint64_t next_id = 1;

            auto measure_avg_ns_at_position = [&](std::size_t target_position) -> double
            {
                double total_ns = 0.0;
                for (int t = 0; t < kTrials; ++t)
                {
                    std::vector<uint64_t> ids;
                    ids.reserve(kLevelDepth);
                    for (int i = 0; i < kLevelDepth; ++i)
                    {
                        const uint64_t id = next_id++;
                        HYDRA_CHECK(f.book->add_order(make_order(id, 100, 1, Side::BUY)) != nullptr);
                        ids.push_back(id);
                    }

                    const uint64_t target_id = ids[target_position];
                    const auto start = std::chrono::steady_clock::now();
                    const bool cancelled = f.book->cancel_order(target_id);
                    const auto end = std::chrono::steady_clock::now();
                    HYDRA_CHECK(cancelled);
                    total_ns += static_cast<double>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());

                    // Drain the rest (untimed) so the next trial starts clean.
                    for (const uint64_t id : ids)
                    {
                        if (id != target_id)
                        {
                            HYDRA_CHECK(f.book->cancel_order(id));
                        }
                    }
                }
                return total_ns / static_cast<double>(kTrials);
            };

            const double avg_head_ns = measure_avg_ns_at_position(0);
            const double avg_tail_ns = measure_avg_ns_at_position(
                static_cast<std::size_t>(kLevelDepth - 1));

            std::fprintf(stderr,
                         "    avg cancel-at-head=%.1f ns, avg cancel-at-tail=%.1f ns "
                         "(FIFO depth=%d, %d trials each)\n",
                         avg_head_ns, avg_tail_ns, kLevelDepth, kTrials);

            const double ratio = avg_tail_ns / avg_head_ns;
            std::fprintf(stderr, "    tail/head ratio=%.2f (expect close to 1.0, not ~%d)\n",
                         ratio, kLevelDepth);

            // A linear-scan cancel would push this ratio toward kLevelDepth
            // (5000); an O(1) cancel keeps it near 1 regardless of noise.
            HYDRA_CHECK(ratio < 3.0);
            HYDRA_CHECK(ratio > 1.0 / 3.0);
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_add_multiple_orders_multiple_price_levels);
    RUN_TEST(test_best_bid_ask_after_cancel_empties_level);
    RUN_TEST(test_fifo_ordering_within_level);
    RUN_TEST(test_cancel_unknown_order_returns_false);
    RUN_TEST(test_cancel_twice_second_call_fails);
    RUN_TEST(test_duplicate_order_id_rejected);
    RUN_TEST(test_level_pool_exhaustion_handled_cleanly);
    RUN_TEST(test_next_level_traversal_visits_in_price_order);
    RUN_TEST(test_cancel_is_position_independent_micro_benchmark);

    return report_and_exit_code();
}
