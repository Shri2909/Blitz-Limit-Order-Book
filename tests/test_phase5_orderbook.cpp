// Order-book insertion, cancellation, level traversal and cancel-scaling tests.

#include "test_fixtures.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

namespace hydra::test
{
    namespace
    {

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

            // A second order at an existing price does not add a level.
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

            // The FIFO chain preserves arrival order.
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

            // Cancelling the middle order relinks the chain.
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
            // The failed cancel leaves the book untouched.
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
            // The original order is not overwritten.
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
                Order *r =
                    f.book->add_order(make_order(i + 1, static_cast<int64_t>(i), 1, Side::BUY));
                HYDRA_CHECK(r != nullptr);
            }
            HYDRA_CHECK_EQ(f.book->bid_level_count(), LEVEL_POOL_SIZE);

            // A new price needs a new level, so this add must fail cleanly.
            Order *overflow = f.book->add_order(
                make_order(999'999, static_cast<int64_t>(LEVEL_POOL_SIZE), 1, Side::BUY));
            HYDRA_CHECK(overflow == nullptr);
            HYDRA_CHECK_EQ(f.book->bid_level_count(), LEVEL_POOL_SIZE);

            // Exhaustion only blocks new levels; an existing price still accepts orders.
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

        // Average time to cancel the order at `position` in a FIFO of `depth` orders at one price.
        double average_cancel_ns(Fixture &f, int depth, std::size_t position, int trials)
        {
            double total_ns = 0.0;
            for (int t = 0; t < trials; ++t)
            {
                for (int i = 0; i < depth; ++i)
                {
                    HYDRA_CHECK(f.book->add_order(make_order(static_cast<uint64_t>(i) + 1, 100, 1,
                                                             Side::BUY)) != nullptr);
                }

                const uint64_t target_id = position + 1;
                const auto start = std::chrono::steady_clock::now();
                const bool cancelled = f.book->cancel_order(target_id);
                const auto end = std::chrono::steady_clock::now();
                HYDRA_CHECK(cancelled);
                total_ns += static_cast<double>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());

                // Drain the remaining orders (untimed) before the next trial.
                for (int i = 0; i < depth; ++i)
                {
                    const uint64_t id = static_cast<uint64_t>(i) + 1;
                    if (id != target_id)
                    {
                        HYDRA_CHECK(f.book->cancel_order(id));
                    }
                }
            }
            return total_ns / static_cast<double>(trials);
        }

        // Cancelling at the head and tail of a long FIFO costs about the same.
        void test_cancel_is_position_independent_micro_benchmark()
        {
            Fixture f;

            constexpr int kLevelDepth = 5000;
            constexpr int kTrials = 100;

            const double avg_head_ns = average_cancel_ns(f, kLevelDepth, 0, kTrials);
            const double avg_tail_ns = average_cancel_ns(
                f, kLevelDepth, static_cast<std::size_t>(kLevelDepth - 1), kTrials);

            std::fprintf(stderr,
                         "    avg cancel-at-head=%.1f ns, avg cancel-at-tail=%.1f ns "
                         "(FIFO depth=%d, %d trials each)\n",
                         avg_head_ns, avg_tail_ns, kLevelDepth, kTrials);

            const double ratio = avg_tail_ns / avg_head_ns;
            std::fprintf(stderr, "    tail/head ratio=%.2f (expect close to 1.0, not ~%d)\n", ratio,
                         kLevelDepth);

            // A FIFO scan would scale with kLevelDepth; allow a 5x ratio for timing noise.
            HYDRA_CHECK(ratio < 5.0);
            HYDRA_CHECK(ratio > 1.0 / 5.0);
        }

        // Cancel cost does not grow with the number of resting orders.
        void test_cancel_latency_is_depth_independent()
        {
            Fixture f;

            constexpr int kTrials = 50;
            constexpr int kLowDepth = 100;
            constexpr int kHighDepth = 40'000;

            const double avg_low_ns = average_cancel_ns(f, kLowDepth, 0, kTrials);
            const double avg_high_ns = average_cancel_ns(f, kHighDepth, 0, kTrials);

            std::fprintf(stderr, "    avg cancel at depth=%d: %.1f ns, at depth=%d: %.1f ns\n",
                         kLowDepth, avg_low_ns, kHighDepth, avg_high_ns);

            // A linear dependency would approach the 400x depth ratio; allow 10x for noise.
            HYDRA_CHECK(avg_high_ns < avg_low_ns * 10.0);
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
    RUN_TEST(test_cancel_latency_is_depth_independent);

    return report_and_exit_code();
}
