//===----------------------------------------------------------------------===
// tests/test_phase4_spsc.cpp
//
// Phase 4 exit condition (see HYDRA-LOB-Roadmap.md):
//   "SPSC stress test passes under TSan with zero races."
//
// WHY this binary is itself built with -fsanitize=thread,undefined (see
// CMakeLists.txt's blitz_lob_test_phase4_spsc target): the exit condition
// explicitly says "under TSan" -- a plain, uninstrumented build can only
// prove the logic *looks* race-free, not that TSan's own happens-before
// analysis found nothing across a real concurrent producer/consumer run.
//
// Cross-thread-safety note for this file itself: worker thread bodies never
// call HYDRA_CHECK (throwing across a thread boundary with no matching
// catch would call std::terminate) -- they only record results into plain
// local state, which the main thread reads *after* joining both threads
// (std::jthread's join establishes happens-before, so no race in the
// reading itself) and asserts on there.
//===----------------------------------------------------------------------===

#include "hydra/spsc_queue.hpp"

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

namespace hydra::test
{
    namespace
    {

        struct StampedValue
        {
            uint64_t seq;
            uint64_t padding[3];
        };

        void test_capacity_reports_correctly()
        {
            SpscQueue<uint64_t, 256> q;
            HYDRA_CHECK_EQ(q.capacity(), std::size_t{256});
            HYDRA_CHECK((q.capacity() & (q.capacity() - 1)) == std::size_t{0});
        }

        void test_single_threaded_fifo_order()
        {
            SpscQueue<uint64_t, 16> q;
            for (uint64_t i = 0; i < 10; ++i)
            {
                HYDRA_CHECK(q.push(i));
            }
            for (uint64_t i = 0; i < 10; ++i)
            {
                uint64_t out = 0;
                HYDRA_CHECK(q.pop(out));
                HYDRA_CHECK_EQ(out, i);
            }
            uint64_t out = 0;
            HYDRA_CHECK(!q.pop(out));
        }

        void test_push_fails_when_full_and_recovers()
        {
            constexpr std::size_t kCapacity = 8;
            SpscQueue<uint64_t, kCapacity> q;

            for (uint64_t i = 0; i < kCapacity; ++i)
            {
                HYDRA_CHECK(q.push(i));
            }
            HYDRA_CHECK(!q.push(999)); // full: must fail cleanly, not overwrite

            uint64_t out = 0;
            HYDRA_CHECK(q.pop(out));
            HYDRA_CHECK_EQ(out, uint64_t{0});

            // One slot freed -- push must succeed again immediately.
            HYDRA_CHECK(q.push(999));
        }

        void test_pop_fails_when_empty()
        {
            SpscQueue<uint64_t, 4> q;
            uint64_t out = 0;
            HYDRA_CHECK(!q.pop(out));
        }

        void test_approx_size_tracks_pushes_and_pops()
        {
            SpscQueue<uint64_t, 16> q;
            HYDRA_CHECK_EQ(q.approx_size(), std::size_t{0});
            for (uint64_t i = 0; i < 5; ++i)
            {
                HYDRA_CHECK(q.push(i));
            }
            HYDRA_CHECK_EQ(q.approx_size(), std::size_t{5});
            uint64_t out = 0;
            HYDRA_CHECK(q.pop(out));
            HYDRA_CHECK(q.pop(out));
            HYDRA_CHECK_EQ(q.approx_size(), std::size_t{3});
        }

        // The core Phase 4 exit-condition test: a real producer std::jthread
        // and consumer std::jthread, pushing/popping as fast as possible
        // against a small (heavily-wrapped) ring buffer, verifying strict
        // ordering with zero drops and zero duplicates end to end.
        void test_concurrent_producer_consumer_strict_ordering()
        {
            constexpr std::size_t kCapacity = 256;
            constexpr uint64_t kItemCount = 200'000;
            // Generous safety bound so a genuine bug (queue stuck full/empty
            // forever) fails the test instead of hanging the binary forever.
            constexpr uint64_t kMaxSpinAttemptsPerItem = 50'000'000;

            SpscQueue<StampedValue, kCapacity> q;

            std::atomic<bool> producer_gave_up{false};
            std::atomic<bool> consumer_gave_up{false};

            std::jthread producer([&](std::stop_token)
                                  {
                for (uint64_t seq = 0; seq < kItemCount; ++seq)
                {
                    const StampedValue item{seq, {0, 0, 0}};
                    uint64_t attempts = 0;
                    while (!q.push(item))
                    {
                        if (++attempts > kMaxSpinAttemptsPerItem)
                        {
                            producer_gave_up.store(true, std::memory_order_relaxed);
                            return;
                        }
                        if (attempts % 4096 == 0)
                        {
                            std::this_thread::yield();
                        }
                    }
                } });

            uint64_t received_count = 0;
            uint64_t expected_next_seq = 0;
            bool ordering_violated = false;
            uint64_t violation_at_index = 0;
            uint64_t violation_expected = 0;
            uint64_t violation_actual = 0;

            std::jthread consumer([&](std::stop_token)
                                  {
                while (received_count < kItemCount)
                {
                    StampedValue item{};
                    uint64_t attempts = 0;
                    while (!q.pop(item))
                    {
                        if (++attempts > kMaxSpinAttemptsPerItem)
                        {
                            consumer_gave_up.store(true, std::memory_order_relaxed);
                            return;
                        }
                        if (attempts % 4096 == 0)
                        {
                            std::this_thread::yield();
                        }
                    }

                    if (!ordering_violated && item.seq != expected_next_seq)
                    {
                        ordering_violated = true;
                        violation_at_index = received_count;
                        violation_expected = expected_next_seq;
                        violation_actual = item.seq;
                    }

                    ++received_count;
                    expected_next_seq = item.seq + 1;
                } });

            producer.join();
            consumer.join();

            HYDRA_CHECK(!producer_gave_up.load());
            HYDRA_CHECK(!consumer_gave_up.load());
            HYDRA_CHECK_EQ(received_count, kItemCount);

            if (ordering_violated)
            {
                std::fprintf(stderr,
                             "    ordering violated at received index %llu: "
                             "expected seq=%llu, got seq=%llu\n",
                             static_cast<unsigned long long>(violation_at_index),
                             static_cast<unsigned long long>(violation_expected),
                             static_cast<unsigned long long>(violation_actual));
            }
            HYDRA_CHECK(!ordering_violated);

            // Queue must be fully drained -- no leftover/duplicated items.
            HYDRA_CHECK_EQ(q.approx_size(), std::size_t{0});
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_capacity_reports_correctly);
    RUN_TEST(test_single_threaded_fifo_order);
    RUN_TEST(test_push_fails_when_full_and_recovers);
    RUN_TEST(test_pop_fails_when_empty);
    RUN_TEST(test_approx_size_tracks_pushes_and_pops);
    RUN_TEST(test_concurrent_producer_consumer_strict_ordering);

    return report_and_exit_code();
}
