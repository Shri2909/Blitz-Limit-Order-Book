#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hydra
{

    template <typename T, std::size_t N> class SpscQueue
    {
        static_assert((N & (N - 1)) == 0, "N must be power of 2");

        static constexpr std::uint64_t kMask = static_cast<std::uint64_t>(N) - 1;

        static_assert(std::is_trivially_copyable_v<T>,
                      "SpscQueue hands T across threads by value; T must be trivially copyable");

        alignas(64) std::array<T, N> buffer_{};
        alignas(64) std::atomic<std::uint64_t> tail_{0};
        alignas(64) std::atomic<std::uint64_t> head_{0};

    public:
        SpscQueue() = default;
        SpscQueue(const SpscQueue &) = delete;
        SpscQueue &operator=(const SpscQueue &) = delete;

        static constexpr std::size_t capacity() noexcept { return N; }

        std::size_t approx_size() const noexcept
        {
            // Diagnostic snapshot only; concurrent updates can make either value stale.
            return static_cast<std::size_t>(tail_.load(std::memory_order_relaxed) -
                                            head_.load(std::memory_order_relaxed));
        }

        [[nodiscard]] bool push(const T &item) noexcept
        {
            // Only the producer writes tail_, so this load needs no synchronization.
            const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
            // Pair with the consumer release before reusing a freed slot.
            const std::uint64_t head = head_.load(std::memory_order_acquire);

            if (tail - head < N) [[likely]]
            {
                buffer_[tail & kMask] = item;
                // Publish the element to the consumer acquire-load of tail_.
                tail_.store(tail + 1, std::memory_order_release);
                return true;
            }
            else [[unlikely]]
            {
                return false;
            }
        }

        [[nodiscard]] bool pop(T &out) noexcept
        {
            // Only the consumer writes head_, so this load needs no synchronization.
            const std::uint64_t head = head_.load(std::memory_order_relaxed);
            // Pair with the producer release to observe the published element.
            const std::uint64_t tail = tail_.load(std::memory_order_acquire);

            if (head < tail) [[likely]]
            {
                out = buffer_[head & kMask];
                // Publish the freed slot after its element has been copied out.
                head_.store(head + 1, std::memory_order_release);
                return true;
            }
            else [[unlikely]]
            {
                return false;
            }
        }
    };

} // namespace hydra
