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
        // Producer-owned line: the published tail plus a private copy of the last
        // head the producer observed. The copy is plain (non-atomic) because only the
        // producer thread reads or writes it.
        alignas(64) std::atomic<std::uint64_t> tail_{0};
        std::uint64_t cached_head_{0};
        // Consumer-owned line: the published head plus a private copy of the last tail.
        alignas(64) std::atomic<std::uint64_t> head_{0};
        std::uint64_t cached_tail_{0};

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

            // cached_head_ is a value this thread earlier read with acquire from head_.
            // head_ only grows, so it is a lower bound on the true head; if it shows
            // room, the slot was already freed (and its copy-out ordered before our
            // write) at that acquire. Only a full-looking ring pays for a fresh load,
            // which keeps the consumer's head_ line in the consumer's cache.
            if (tail - cached_head_ >= N) [[unlikely]]
            {
                // Pair with the consumer release before reusing a freed slot.
                cached_head_ = head_.load(std::memory_order_acquire);
                if (tail - cached_head_ >= N)
                {
                    return false;
                }
            }

            buffer_[tail & kMask] = item;
            // Publish the element to the consumer acquire-load of tail_.
            tail_.store(tail + 1, std::memory_order_release);
            return true;
        }

        [[nodiscard]] bool pop(T &out) noexcept
        {
            // Only the consumer writes head_, so this load needs no synchronization.
            const std::uint64_t head = head_.load(std::memory_order_relaxed);

            // cached_tail_ is a value this thread earlier read with acquire from tail_;
            // tail_ only grows, so every slot below it was published before that acquire.
            // The cross-core tail_ load is skipped whenever a backlog is known to exist.
            if (head == cached_tail_)
            {
                // Pair with the producer release to observe the published element.
                cached_tail_ = tail_.load(std::memory_order_acquire);
                if (head == cached_tail_) [[unlikely]]
                {
                    return false;
                }
            }

            out = buffer_[head & kMask];
            // Publish the freed slot after its element has been copied out.
            head_.store(head + 1, std::memory_order_release);
            return true;
        }
    };

} // namespace hydra
