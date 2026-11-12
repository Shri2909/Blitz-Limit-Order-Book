#pragma once

// Fixed-capacity pool with an inline slab. Production capacities consume
// several MiB; allocate large pools on the heap to avoid stack overflow.

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

    template <typename T, std::size_t N> class ObjectPool
    {
        static_assert(N > 0, "ObjectPool capacity N must be positive");

        // Single-threaded ownership: acquire(), release(), and reset() must be
        // called by the owning matching thread or while it is idle. The free list
        // is non-atomic; only exhaustion telemetry is atomic.
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
                          "acquire() is noexcept; T must be nothrow default-constructible");

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
        [[nodiscard]] T *
        acquire(Args &&...args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
        {
            static_assert(std::is_nothrow_constructible_v<T, Args...>,
                          "acquire() is noexcept; T must be nothrow-constructible from Args");

            Slot *const slot = free_head_;
            if (slot == nullptr) [[unlikely]]
            {
                exhaustion_count_.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            else [[likely]]
            {
                free_head_ = slot->next;
                return ::new (static_cast<void *>(&slot->obj)) T(std::forward<Args>(args)...);
            }
        }

        void release(T *ptr) noexcept
        {
            static_assert(std::is_nothrow_destructible_v<T>,
                          "release() is noexcept; T's destructor must be nothrow");
            assert(ptr != nullptr && "ObjectPool::release() called with a null pointer");
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

        // Rebuild the free list without destroying outstanding objects. Requires
        // trivially destructible T and no concurrent pool users. The caller must
        // discard all outstanding references first. Exhaustion telemetry remains
        // cumulative across resets.
        void reset() noexcept
        {
            static_assert(std::is_trivially_destructible_v<T>,
                          "reset() skips destructors; T must be trivially destructible");
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
