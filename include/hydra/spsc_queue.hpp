#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hydra
{

    template <typename T, std::size_t N>
    class SpscQueue
    {
        static_assert((N & (N - 1)) == 0, "N must be power of 2");

        static constexpr std::uint64_t kMask = static_cast<std::uint64_t>(N) - 1;

        static_assert(std::is_trivially_copyable_v<T>,
                      "SpscQueue moves T across threads by value; T must be "
                      "trivially copyable so assignment is a safe, tearing-free "
                      "cross-thread handoff (see Order's own static_assert in "
                      "types.hpp for the same contract)");

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
            // Both relaxed: this is an approximate/diagnostic read (hence the
            // name) with no code depending on it observing a consistent
            // snapshot of the two counters relative to each other -- it may
            // race with a concurrent push()/pop() and return a stale value,
            // which is fine for its intended use (telemetry, not control flow).
            return static_cast<std::size_t>(
                tail_.load(std::memory_order_relaxed) -
                head_.load(std::memory_order_relaxed));
        }

        [[nodiscard]] bool push(const T &item) noexcept
        {
            // Relaxed: only this thread (the producer) ever writes tail_, so
            // a relaxed load is guaranteed to see our own most recent write
            // back -- no cross-thread visibility is needed for this read.
            const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
            // Acquire: must see the consumer's most recent release-store of
            // head_ (i.e. its most recently freed slot) before deciding
            // whether there's room, or we could read a stale head_, conclude
            // "full" against room the consumer already made, and drop an
            // item unnecessarily -- or worse, judge "room" against a head_
            // that hasn't yet caught up, and overwrite a slot the consumer
            // hasn't finished reading (see pop()'s matching release below).
            const std::uint64_t head = head_.load(std::memory_order_acquire);

            if (tail - head < N) [[likely]]
            {
                buffer_[tail & kMask] = item;
                // Release: publishes the buffer_ write above. pop()'s
                // acquire-load of tail_ is guaranteed to see this element
                // write (not a torn/earlier value) once it observes the new
                // tail_ -- this is the release/acquire pair this queue's
                // entire cross-thread safety rests on.
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
            // Relaxed: only this thread (the consumer) ever writes head_, so
            // a relaxed load is guaranteed to see our own most recent write
            // back -- symmetric with push()'s relaxed read of tail_ above.
            const std::uint64_t head = head_.load(std::memory_order_relaxed);
            // Acquire: pairs with push()'s release-store of tail_ above,
            // guaranteeing the buffer_ write for this slot is visible to us
            // before we read it -- without this, we could observe the new
            // tail_ value but still read stale/uninitialized buffer_ content
            // due to reordering.
            const std::uint64_t tail = tail_.load(std::memory_order_acquire);

            if (head < tail) [[likely]]
            {
                out = buffer_[head & kMask];
                // Release: publishes "this slot is free" -- push()'s
                // acquire-load of head_ will see it before deciding it's
                // safe to reuse the slot, preventing the producer from
                // overwriting an element the consumer hasn't finished
                // copying out yet.
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
