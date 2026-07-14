// metrics/src/pool_exhaustion.cpp
//
// Metric A5: ObjectPool exhaustion and recovery proof.
//
// Drains a small pool completely (recording exhaustion_count() after each
// acquire), confirms it stays at zero right up through the very last
// successful acquire, then shows it start incrementing exactly on the
// first over-capacity attempt and beyond -- and that releasing a single
// slot immediately makes the pool usable again (exhaustion_count stops
// climbing, the next acquire succeeds).
//
// Uses a small local capacity (not the real ORDER_POOL_SIZE=65536) purely
// so the exhaustion boundary is visible on a chart at a legible scale.
//
// Output: CSV to stdout -- event_index,operation,acquire_ok,exhaustion_count

#include "hydra/object_pool.hpp"

#include <cstdio>
#include <memory>
#include <vector>

using namespace hydra;

namespace
{
    struct Widget
    {
        uint64_t tag = 0;
    };
}

int main()
{
    constexpr std::size_t kCapacity = 20;
    constexpr int kOverAcquireAttempts = 8;

    auto pool = std::make_unique<ObjectPool<Widget, kCapacity>>();

    std::printf("event_index,operation,acquire_ok,exhaustion_count\n");

    int event_index = 0;
    std::vector<Widget *> held;

    for (std::size_t i = 0; i < kCapacity; ++i)
    {
        Widget *w = pool->acquire();
        std::printf("%d,acquire,%d,%llu\n", event_index++, w != nullptr ? 1 : 0,
                    static_cast<unsigned long long>(pool->exhaustion_count()));
        held.push_back(w);
    }

    for (int i = 0; i < kOverAcquireAttempts; ++i)
    {
        Widget *w = pool->acquire();
        std::printf("%d,acquire_over_capacity,%d,%llu\n", event_index++, w != nullptr ? 1 : 0,
                    static_cast<unsigned long long>(pool->exhaustion_count()));
    }

    // Release exactly one -- pool must recover immediately.
    pool->release(held.back());
    held.pop_back();
    std::printf("%d,release,1,%llu\n", event_index++,
                static_cast<unsigned long long>(pool->exhaustion_count()));

    Widget *recovered = pool->acquire();
    std::printf("%d,acquire_after_release,%d,%llu\n", event_index++, recovered != nullptr ? 1 : 0,
                static_cast<unsigned long long>(pool->exhaustion_count()));

    for (Widget *w : held)
    {
        pool->release(w);
    }
    if (recovered != nullptr)
    {
        pool->release(recovered);
    }

    return 0;
}
