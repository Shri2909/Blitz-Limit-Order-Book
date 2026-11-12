// Object-pool reuse, exhaustion and lifetime tests (built with ASan/UBSan).

#include "hydra/object_pool.hpp"

#include "test_harness.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace hydra::test
{
    namespace
    {

        struct Widget
        {
            uint64_t tag = 0;
            uint64_t payload[7] = {};
        };

        // Small capacity forces slot reuse during the 100k-cycle test.
        constexpr std::size_t kSmallCapacity = 64;

        void test_acquire_release_cycle_100k()
        {
            auto pool = std::make_unique<ObjectPool<Widget, kSmallCapacity>>();

            constexpr int kIterations = 100'000;
            for (int i = 0; i < kIterations; ++i)
            {
                Widget *w = pool->acquire();
                HYDRA_CHECK(w != nullptr);

                w->tag = static_cast<uint64_t>(i);
                HYDRA_CHECK_EQ(w->tag, static_cast<uint64_t>(i));

                pool->release(w);
            }
            HYDRA_CHECK_EQ(pool->exhaustion_count(), uint64_t{0});
        }

        void test_exhaustion_returns_nullptr_without_crash()
        {
            constexpr std::size_t kCapacity = 32;
            auto pool = std::make_unique<ObjectPool<Widget, kCapacity>>();

            std::vector<Widget *> held;
            held.reserve(kCapacity);
            for (std::size_t i = 0; i < kCapacity; ++i)
            {
                Widget *w = pool->acquire();
                HYDRA_CHECK(w != nullptr);
                held.push_back(w);
            }
            HYDRA_CHECK_EQ(pool->exhaustion_count(), uint64_t{0});

            // Every acquire on an exhausted pool returns nullptr and is counted.
            for (int attempt = 1; attempt <= 5; ++attempt)
            {
                Widget *overflow = pool->acquire();
                HYDRA_CHECK(overflow == nullptr);
                HYDRA_CHECK_EQ(pool->exhaustion_count(), static_cast<uint64_t>(attempt));
            }

            for (Widget *w : held)
            {
                pool->release(w);
            }
        }

        void test_release_and_reacquire_after_exhaustion()
        {
            constexpr std::size_t kCapacity = 8;
            auto pool = std::make_unique<ObjectPool<Widget, kCapacity>>();

            std::vector<Widget *> held;
            for (std::size_t i = 0; i < kCapacity; ++i)
            {
                held.push_back(pool->acquire());
            }
            HYDRA_CHECK(pool->acquire() == nullptr);

            // One released slot makes the pool usable again.
            Widget *const released = held.back();
            held.pop_back();
            pool->release(released);

            Widget *const reacquired = pool->acquire();
            HYDRA_CHECK(reacquired != nullptr);
            // The freelist is LIFO: the released slot is handed out next.
            HYDRA_CHECK_EQ(reacquired, released);

            held.push_back(reacquired);
            for (Widget *w : held)
            {
                pool->release(w);
            }
        }

        struct DtorCounter
        {
            static inline int live_count = 0;

            DtorCounter() noexcept { ++live_count; }
            ~DtorCounter() noexcept { --live_count; }
            DtorCounter(const DtorCounter &) = delete;
            DtorCounter &operator=(const DtorCounter &) = delete;
        };

        void test_release_calls_destructor()
        {
            auto pool = std::make_unique<ObjectPool<DtorCounter, 16>>();
            DtorCounter::live_count = 0;

            DtorCounter *a = pool->acquire();
            DtorCounter *b = pool->acquire();
            HYDRA_CHECK(a != nullptr && b != nullptr);
            HYDRA_CHECK_EQ(DtorCounter::live_count, 2);

            pool->release(a);
            HYDRA_CHECK_EQ(DtorCounter::live_count, 1);

            pool->release(b);
            HYDRA_CHECK_EQ(DtorCounter::live_count, 0);
        }

        struct TaggedWidget
        {
            uint64_t tag;
            explicit TaggedWidget(uint64_t t) noexcept : tag(t) {}
        };

        void test_acquire_with_constructor_args()
        {
            auto pool = std::make_unique<ObjectPool<TaggedWidget, 4>>();

            TaggedWidget *w = pool->acquire(uint64_t{42});
            HYDRA_CHECK(w != nullptr);
            HYDRA_CHECK_EQ(w->tag, uint64_t{42});
            pool->release(w);
        }

        void test_100k_cycle_leaves_no_leaked_liveness()
        {
            // Draining the pool after reuse must still yield its full capacity.
            constexpr std::size_t kCapacity = 128;
            auto pool = std::make_unique<ObjectPool<Widget, kCapacity>>();

            for (int i = 0; i < 100'000; ++i)
            {
                Widget *w = pool->acquire();
                HYDRA_CHECK(w != nullptr);
                pool->release(w);
            }

            std::vector<Widget *> drained;
            for (std::size_t i = 0; i < kCapacity; ++i)
            {
                Widget *w = pool->acquire();
                HYDRA_CHECK(w != nullptr);
                drained.push_back(w);
            }
            HYDRA_CHECK(pool->acquire() == nullptr);

            for (Widget *w : drained)
            {
                pool->release(w);
            }
        }

    } // namespace
} // namespace hydra::test

int main()
{
    using namespace hydra::test;

    RUN_TEST(test_acquire_release_cycle_100k);
    RUN_TEST(test_exhaustion_returns_nullptr_without_crash);
    RUN_TEST(test_release_and_reacquire_after_exhaustion);
    RUN_TEST(test_release_calls_destructor);
    RUN_TEST(test_acquire_with_constructor_args);
    RUN_TEST(test_100k_cycle_leaves_no_leaked_liveness);

    return report_and_exit_code();
}
