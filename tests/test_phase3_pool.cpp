//===----------------------------------------------------------------------===
// tests/test_phase3_pool.cpp
//
// Phase 3 exit condition (see HYDRA-LOB-Roadmap.md):
//   "pool acquires/releases 100k objects with zero leaks under ASan;
//   nullptr returned cleanly on overflow."
//
// WHY this binary is itself built with -fsanitize=address,undefined (see
// CMakeLists.txt's blitz_lob_test_phase3_pool target): the exit condition
// explicitly says "under ASan" -- a plain, uninstrumented build proves the
// pool's *logic* is right but cannot prove "zero leaks reported" the way
// the phase actually requires. Every acquire()/release() below runs with
// the ASan runtime live, so a real leak, double-free, or out-of-slab write
// aborts the process with ASan's own diagnostic, not a silent pass.
//
// Uses a local test-only type (Widget), not hydra::Order/Level/FillEvent --
// ObjectPool<T,N> is generic over T, and this phase is about the pool's own
// correctness, independent of any domain type (see Phase 1's/Phase 5's
// files for domain-type-specific coverage).
//===----------------------------------------------------------------------===

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

        // WHY 64: small enough that a 100k-iteration acquire/release loop
        // forces genuine slot reuse many times over (100,000 / 64 ≈ 1,563
        // full cycles through the freelist), which is exactly what "well
        // past N to exercise reuse" calls for.
        constexpr std::size_t kSmallCapacity = 64;

        void test_acquire_release_cycle_100k()
        {
            auto pool = std::make_unique<ObjectPool<Widget, kSmallCapacity>>();

            constexpr int kIterations = 100'000;
            for (int i = 0; i < kIterations; ++i)
            {
                Widget *w = pool->acquire();
                HYDRA_CHECK(w != nullptr);

                // Write through the freshly (re)acquired slot -- if release()
                // ever failed to hand back a genuinely reusable slot, or if
                // acquire() handed out a slot still aliased by a previous
                // caller, ASan's shadow memory would catch the resulting
                // write as a use-after-free/heap-buffer-overflow here.
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

            // Genuinely exhausted now (drained without releasing, per the
            // spec's validation step) -- every further acquire() must
            // return nullptr cleanly, never crash, and keep counting.
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

            // Release exactly one slot back, confirm capacity is
            // immediately recoverable (not permanently "stuck" exhausted).
            Widget *const released = held.back();
            held.pop_back();
            pool->release(released);

            Widget *const reacquired = pool->acquire();
            HYDRA_CHECK(reacquired != nullptr);
            // LIFO freelist: the slot just released must be the next one
            // handed back out.
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
            // Independent of ASan's own leak detector: verify in-process
            // that a full 100k acquire/release cycle leaves the pool's
            // notion of "live objects" at exactly zero by draining every
            // slot afterward and confirming the full capacity is available
            // again -- if any earlier iteration had leaked a slot (freed
            // without going back on the freelist), this would come up short.
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
