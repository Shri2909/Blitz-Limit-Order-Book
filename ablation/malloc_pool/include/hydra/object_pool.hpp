#pragma once

// ablation/malloc_pool/include/hydra/object_pool.hpp
//
// Rejected-alternative ablation for the fixed-slab object pool
// (include/hydra/object_pool.hpp): acquire()/release() call
// ::new(std::nothrow)/delete directly, bypassing the free-list slab
// entirely, while keeping the exact same public API (acquire(),
// acquire(Args&&...), release(), exhaustion_count()) so every caller
// (order_book.hpp, matcher.hpp, pipeline construction in main.cpp) compiles
// unmodified against it. exhaustion_count() only increments on a genuine
// allocation failure here (vanishingly rare for a POD this size), unlike
// the real pool where it means "the fixed slab is full" -- kept for
// interface parity, not because it's expected to ever be nonzero.
//
// See spsc_queue.hpp's ablation counterpart for the include-path-shadowing
// mechanism this relies on.

#include <atomic>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace hydra
{

    template <typename T, std::size_t N>
    class ObjectPool
    {
        static_assert(N > 0, "ObjectPool capacity N must be positive");

    public:
        ObjectPool() noexcept = default;
        ~ObjectPool() = default;

        ObjectPool(const ObjectPool &) = delete;
        ObjectPool &operator=(const ObjectPool &) = delete;
        ObjectPool(ObjectPool &&) = delete;
        ObjectPool &operator=(ObjectPool &&) = delete;

        [[nodiscard]] T *acquire() noexcept
        {
            static_assert(std::is_nothrow_default_constructible_v<T>,
                          "T must be nothrow default-constructible");
            T *const p = ::new (std::nothrow) T();
            if (p == nullptr) [[unlikely]]
            {
                exhaustion_count_.fetch_add(1, std::memory_order_relaxed);
            }
            return p;
        }

        template <typename... Args>
            requires(sizeof...(Args) > 0)
        [[nodiscard]] T *acquire(Args &&...args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
        {
            static_assert(std::is_nothrow_constructible_v<T, Args...>,
                          "T's constructor for these Args must be nothrow");
            T *const p = ::new (std::nothrow) T(std::forward<Args>(args)...);
            if (p == nullptr) [[unlikely]]
            {
                exhaustion_count_.fetch_add(1, std::memory_order_relaxed);
            }
            return p;
        }

        void release(T *ptr) noexcept
        {
            static_assert(std::is_nothrow_destructible_v<T>,
                          "T's destructor must be nothrow");
            delete ptr;
        }

        [[nodiscard]] uint64_t exhaustion_count() const noexcept
        {
            return exhaustion_count_.load(std::memory_order_relaxed);
        }

        void reset() noexcept
        {
            // No-op: this ablation has no free-list/slab state to rebuild
            // -- every acquire() is a fresh ::new, every release() a plain
            // delete, so there's nothing to reset between trials. Exists
            // only so run_benchmark()'s between-trials reset() call (see
            // the real include/hydra/object_pool.hpp's own WHY) compiles
            // against this shadowed pool too -- keeping this ablation's
            // public API in sync with the real one is the whole point of
            // the shadowing mechanism (see spsc_queue.hpp's WHY).
        }

    private:
        std::atomic<uint64_t> exhaustion_count_{0};
    };

} // namespace hydra
