#pragma once

// Allocation ablation: same ObjectPool API backed by new/delete per object.

#include <atomic>
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
        [[nodiscard]] T *
        acquire(Args &&...args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
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
            static_assert(std::is_nothrow_destructible_v<T>, "T's destructor must be nothrow");
            delete ptr;
        }

        [[nodiscard]] uint64_t exhaustion_count() const noexcept
        {
            return exhaustion_count_.load(std::memory_order_relaxed);
        }

        // No slab to rebuild; kept for API compatibility.
        void reset() noexcept {}

    private:
        std::atomic<uint64_t> exhaustion_count_{0};
    };

} // namespace hydra
