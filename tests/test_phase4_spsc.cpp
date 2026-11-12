// SPSC queue ordering, full/empty and concurrency tests (built with TSan).
// Worker threads only record results; assertions run after join.

#include "hydra/spsc_queue.hpp"

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
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
            HYDRA_CHECK(!q.push(999)); // full: fails without overwriting

            uint64_t out = 0;
            HYDRA_CHECK(q.pop(out));
            HYDRA_CHECK_EQ(out, uint64_t{0});

            // A freed slot makes push succeed again.
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

        Order make_order(uint64_t i)
        {
            Order o{};
            o.order_id = i + 1;
            o.price = 100'000 + static_cast<int64_t>(i % 977) - 400;
            o.qty = static_cast<uint32_t>(i % 1000);
            o.side = (i % 2 == 0) ? Side::BUY : Side::SELL;
            o.tif = static_cast<TimeInForce>(i % 3);
            o.event_tag = (i % 5 == 0) ? OrderEventTag::REPLACE : OrderEventTag::NEW_OR_CANCEL;
            o.client_id = 1 + i % 8;
            o.timestamp_ns = 0x1234'5678'9abc'0000ULL + i;
            return o;
        }

        bool same_transported_fields(const Order &a, const Order &b)
        {
            return a.order_id == b.order_id && a.price == b.price && a.qty == b.qty &&
                   a.side == b.side && a.tif == b.tif && a.event_tag == b.event_tag &&
                   a.client_id == b.client_id && a.timestamp_ns == b.timestamp_ns;
        }

        // The Order queue carries every field the pipeline reads, across several laps,
        // and hands back zeroed link/tag/padding bytes.
        void test_order_queue_round_trips_fields_over_many_laps()
        {
            constexpr std::size_t kCapacity = 8;
            SpscQueue<Order, kCapacity> q;
            uint64_t next_in = 0;
            uint64_t next_out = 0;
            for (int round = 0; round < 50; ++round)
            {
                for (std::size_t k = 0; k < 5; ++k)
                {
                    HYDRA_CHECK(q.push(make_order(next_in++)));
                }
                for (std::size_t k = 0; k < 5; ++k)
                {
                    Order out{};
                    out.client_tag[0] = 'x'; // stale content must not survive a pop
                    out.prev_ = &out;
                    HYDRA_CHECK(q.pop(out));
                    HYDRA_CHECK(same_transported_fields(out, make_order(next_out++)));
                    HYDRA_CHECK(out.prev_ == nullptr && out.next_ == nullptr);
                    HYDRA_CHECK_EQ(out.client_tag[0], '\0');
                }
                Order none{};
                HYDRA_CHECK(!q.pop(none));
            }
            // Fill to capacity: the (capacity+1)th push fails and nothing is overwritten.
            for (std::size_t k = 0; k < kCapacity; ++k)
            {
                HYDRA_CHECK(q.push(make_order(next_in++)));
            }
            HYDRA_CHECK(!q.push(make_order(next_in)));
            HYDRA_CHECK_EQ(q.approx_size(), kCapacity);
            for (std::size_t k = 0; k < kCapacity; ++k)
            {
                Order out{};
                HYDRA_CHECK(q.pop(out));
                HYDRA_CHECK(same_transported_fields(out, make_order(next_out++)));
            }
        }

        void test_order_queue_concurrent_strict_ordering()
        {
            constexpr std::size_t kCapacity = 256;
            constexpr uint64_t kItemCount = 300'000;
            SpscQueue<Order, kCapacity> q;
            std::atomic<uint64_t> bad{0};
            std::atomic<uint64_t> received{0};

            std::jthread producer(
                [&](std::stop_token)
                {
                    for (uint64_t i = 0; i < kItemCount; ++i)
                    {
                        const Order o = make_order(i);
                        uint64_t attempts = 0;
                        while (!q.push(o))
                        {
                            if (++attempts % 4096 == 0)
                            {
                                std::this_thread::yield();
                            }
                            if (attempts > 50'000'000)
                            {
                                return;
                            }
                        }
                    }
                });
            std::jthread consumer(
                [&](std::stop_token)
                {
                    uint64_t expected = 0;
                    while (expected < kItemCount)
                    {
                        Order out{};
                        uint64_t attempts = 0;
                        while (!q.pop(out))
                        {
                            if (++attempts % 4096 == 0)
                            {
                                std::this_thread::yield();
                            }
                            if (attempts > 50'000'000)
                            {
                                received.store(expected);
                                return;
                            }
                        }
                        if (!same_transported_fields(out, make_order(expected)))
                        {
                            bad.fetch_add(1, std::memory_order_relaxed);
                        }
                        ++expected;
                    }
                    received.store(expected);
                });
            producer.join();
            consumer.join();
            HYDRA_CHECK_EQ(received.load(), kItemCount);
            HYDRA_CHECK_EQ(bad.load(), uint64_t{0});
        }

        // A concurrent producer and consumer preserve every value, in order.
        void test_concurrent_producer_consumer_strict_ordering()
        {
            constexpr std::size_t kCapacity = 256;
            constexpr uint64_t kItemCount = 200'000;
            // Spin bound so a stuck queue fails the test instead of hanging it.
            constexpr uint64_t kMaxSpinAttemptsPerItem = 50'000'000;

            SpscQueue<StampedValue, kCapacity> q;

            std::atomic<bool> producer_gave_up{false};
            std::atomic<bool> consumer_gave_up{false};

            std::jthread producer(
                [&](std::stop_token)
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
                    }
                });

            uint64_t received_count = 0;
            uint64_t expected_next_seq = 0;
            bool ordering_violated = false;
            uint64_t violation_at_index = 0;
            uint64_t violation_expected = 0;
            uint64_t violation_actual = 0;

            std::jthread consumer(
                [&](std::stop_token)
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
                    }
                });

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

            // The queue is fully drained.
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
    RUN_TEST(test_order_queue_round_trips_fields_over_many_laps);
    RUN_TEST(test_order_queue_concurrent_strict_ordering);
    RUN_TEST(test_concurrent_producer_consumer_strict_ordering);

    return report_and_exit_code();
}
