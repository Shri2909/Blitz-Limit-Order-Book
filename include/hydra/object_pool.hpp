#pragma once

// include/hydra/object_pool.hpp
//
// WARNING: ObjectPool<T,N> embeds its full N-element slab inline (no heap
// allocation). For the pool sizes in config.hpp this is several MiB --
// always heap-allocate (std::make_unique<ObjectPool<...>>()), never declare
// one as a stack local or a by-value function parameter, or you will
// overflow a default 8 MiB thread stack.

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
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

        // WHY plain (non-atomic) free_head_: every ObjectPool instance in
        // this codebase is owned by exactly one OrderBook/Matcher, which is
        // touched exclusively by the matching thread (see pipeline.hpp's
        // wiring policy -- rx_thread_fn only ever pushes onto the SPSC
        // queue, it never touches a pool directly). Given that single-
        // threaded-per-pool-instance invariant, a plain pointer is correct
        // and cheaper than an atomic. If that invariant ever changes and a
        // pool needs acquire()/release() from more than one thread, this
        // must become std::atomic<Slot*> with a proper lock-free free-list
        // protocol (e.g. a tagged-pointer CAS loop to avoid ABA) -- a plain
        // pointer swap is not safe under concurrent access.
        union Slot
        {
            T obj;
            Slot *next;

            Slot() noexcept {}
            ~Slot() noexcept {}

            Slot(const Slot &) = delete;
            Slot &operator=(const Slot &) = delete;
        };

    public:
        ObjectPool() noexcept
        {
            for (std::size_t i = 0; i + 1 < N; ++i)
            {
                slab_[i].next = &slab_[i + 1];
            }
            slab_[N - 1].next = nullptr;
            free_head_ = &slab_[0];
        }

        ~ObjectPool() = default;

        ObjectPool(const ObjectPool &) = delete;
        ObjectPool &operator=(const ObjectPool &) = delete;
        ObjectPool(ObjectPool &&) = delete;
        ObjectPool &operator=(ObjectPool &&) = delete;

        [[nodiscard]] T *acquire() noexcept
        {
            static_assert(std::is_nothrow_default_constructible_v<T>,
                          "T must be nothrow default-constructible: "
                          "ObjectPool::acquire() is noexcept and would "
                          "std::terminate on a throwing constructor");

            Slot *const slot = free_head_;
            if (slot == nullptr) [[unlikely]]
            {
                exhaustion_count_.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            else [[likely]]
            {
                free_head_ = slot->next;
                return ::new (static_cast<void *>(&slot->obj)) T();
            }
        }

        template <typename... Args>
            requires(sizeof...(Args) > 0)
        [[nodiscard]] T *acquire(Args &&...args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
        {
            static_assert(std::is_nothrow_constructible_v<T, Args...>,
                          "T's constructor for these Args must be nothrow: "
                          "this acquire() overload is noexcept(...) and would "
                          "std::terminate on a throwing constructor");

            Slot *const slot = free_head_;
            if (slot == nullptr) [[unlikely]]
            {
                exhaustion_count_.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            else [[likely]]
            {
                free_head_ = slot->next;
                return ::new (static_cast<void *>(&slot->obj))
                    T(std::forward<Args>(args)...);
            }
        }

        void release(T *ptr) noexcept
        {
            static_assert(std::is_nothrow_destructible_v<T>,
                          "T's destructor must be nothrow: release() is "
                          "noexcept and would std::terminate otherwise");
            assert(ptr != nullptr &&
                   "ObjectPool::release() called with a null pointer");
            assert(reinterpret_cast<const std::byte *>(ptr) >=
                       reinterpret_cast<const std::byte *>(slab_.data()) &&
                   reinterpret_cast<const std::byte *>(ptr) <
                       reinterpret_cast<const std::byte *>(slab_.data() + N) &&
                   "ObjectPool::release() called with a pointer this pool did not allocate");

            ptr->~T();

            Slot *const slot = reinterpret_cast<Slot *>(ptr);
            slot->next = free_head_;
            free_head_ = slot;
        }

        [[nodiscard]] uint64_t exhaustion_count() const noexcept
        {
            return exhaustion_count_.load(std::memory_order_relaxed);
        }

        // Rebuilds the free list from scratch, treating every slot as free
        // again -- WITHOUT calling ~T() on whatever might still be
        // "acquired" in those slots. Safe only because every T this pool is
        // instantiated with in this codebase (Order/Level/FillEvent) is
        // trivially destructible (each has its own static_assert to that
        // effect in types.hpp) -- skipping the destructor call is a true
        // no-op for these types, not a leak or a skipped side effect. The
        // static_assert below turns "someone instantiates ObjectPool with a
        // non-trivial T" into a compile error here instead of a silent
        // correctness bug.
        //
        // WHY this exists: run_benchmark()'s trial loop needs each trial to
        // replay the identical dataset against a genuinely clean pool (see
        // OrderBook::reset()'s own WHY for the full story) without paying
        // the cost -- and, more importantly, the reference-stability
        // requirement PipelineContext imposes -- of destroying and
        // reconstructing the whole ObjectPool between trials.
        //
        // Deliberately does NOT reset exhaustion_count_: "did the pool ever
        // run out over the whole run" stays the more useful, cumulative
        // signal, matching how it's already read once at the end of a full
        // run rather than per-trial.
        //
        // Caller's responsibility, not enforced here: only call this when
        // nothing else is concurrently acquiring/releasing from this pool
        // (e.g. after confirming the consumer thread has drained and is
        // idle -- see the call site in benchmark.cpp).
        void reset() noexcept
        {
            static_assert(std::is_trivially_destructible_v<T>,
                          "ObjectPool::reset() skips destructor calls on any "
                          "still-\"acquired\" slot -- only safe if T is "
                          "trivially destructible; revisit this method if "
                          "that ever stops being true for some T");
            for (std::size_t i = 0; i + 1 < N; ++i)
            {
                slab_[i].next = &slab_[i + 1];
            }
            slab_[N - 1].next = nullptr;
            free_head_ = &slab_[0];
        }

    private:
        alignas(64) std::array<Slot, N> slab_;
        Slot *free_head_;
        std::atomic<uint64_t> exhaustion_count_{0};
    };

} // namespace hydra
