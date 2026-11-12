#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "hydra/types.hpp"

namespace hydra
{

    template <typename T, std::size_t N> class SpscQueue
    {
        static_assert(N > 0, "N must be positive");
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

    // SpscQueue<Order, N> for the production Order layout (128 bytes, 64-byte aligned):
    // the pipeline's queue. Other Order layouts (the flat-layout ablation) use the generic
    // queue above. One 64-byte slot per order instead of
    // a 128-byte copy of the whole Order.
    //
    // Why: with a separate tail_ index and a two-line payload, the consumer needs one
    // cross-core transfer to learn that an element exists (tail_) and then more to
    // fetch the payload (two lines). Here the slot's own sequence word is the "ready"
    // flag and shares its cache line with every field the matching engine reads, so
    // the consumer polls the line that will carry the data: one transfer in total.
    // The ring also halves from 8 MiB to 4 MiB at SPSC_CAPACITY = 65536.
    //
    // Transported fields: order_id, price, qty, side, tif, event_tag, client_id,
    // timestamp_ns. Not transported (zeroed on pop): prev_/next_ (book-internal links,
    // never valid across threads), client_tag (read only by Matcher::replace(), copied
    // from a resting order; every dataset and the RX thread leave it zero) and the
    // padding bytes. static_asserts below fail the build if Order gains a field that
    // this slot does not carry.
    template <typename T, std::size_t N>
        requires(std::is_same_v<T, Order> && sizeof(T) == 128 && alignof(T) == 64)
    class SpscQueue<T, N>
    {
        static_assert((N & (N - 1)) == 0, "N must be power of 2");

        static constexpr std::uint64_t kMask = static_cast<std::uint64_t>(N) - 1;

        struct alignas(64) Slot
        {
            // 0 = never written; tail + 1 once the element published at ring position
            // `tail` is complete. Each lap publishes a distinct value.
            std::atomic<std::uint64_t> seq{0};
            std::uint64_t order_id{0};
            std::int64_t price{0};
            std::uint64_t timestamp_ns{0};
            std::uint64_t client_id{0};
            std::uint32_t qty{0};
            Side side{};
            TimeInForce tif{};
            OrderEventTag event_tag{};
        };
        static_assert(sizeof(Slot) == 64, "an order slot must be exactly one cache line");

        // Offsets of the fields the slot carries, and of the ones it deliberately drops.
        static_assert(offsetof(T, order_id) == 0 && offsetof(T, price) == 8 &&
                          offsetof(T, qty) == 16 && offsetof(T, side) == 20 &&
                          offsetof(T, tif) == 21 && offsetof(T, event_tag) == 22 &&
                          offsetof(T, prev_) == 48 && offsetof(T, next_) == 56 &&
                          offsetof(T, timestamp_ns) == 64 && offsetof(T, client_id) == 72 &&
                          offsetof(T, client_tag) == 80 && offsetof(T, cold_padding) == 112,
                      "Order layout changed: update SpscQueue<Order, N> to carry the new field");

        alignas(64) std::array<Slot, N> buffer_{};
        // Producer-owned line: private position (also read by approx_size) and the last
        // head the producer observed.
        alignas(64) std::atomic<std::uint64_t> tail_{0};
        std::uint64_t cached_head_{0};
        // Consumer-owned line. The producer reads it only when the ring looks full.
        alignas(64) std::atomic<std::uint64_t> head_{0};

    public:
        SpscQueue() = default;
        SpscQueue(const SpscQueue &) = delete;
        SpscQueue &operator=(const SpscQueue &) = delete;

        static constexpr std::size_t capacity() noexcept { return N; }

        std::size_t approx_size() const noexcept
        {
            return static_cast<std::size_t>(tail_.load(std::memory_order_relaxed) -
                                            head_.load(std::memory_order_relaxed));
        }

        [[nodiscard]] bool push(const T &item) noexcept
        {
            const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
            if (tail - cached_head_ >= N) [[unlikely]]
            {
                // Pair with the consumer's release before reusing a freed slot.
                cached_head_ = head_.load(std::memory_order_acquire);
                if (tail - cached_head_ >= N)
                {
                    return false;
                }
            }

            Slot &slot = buffer_[tail & kMask];
            slot.order_id = item.order_id;
            slot.price = item.price;
            slot.timestamp_ns = item.timestamp_ns;
            slot.client_id = item.client_id;
            slot.qty = item.qty;
            slot.side = item.side;
            slot.tif = item.tif;
            slot.event_tag = item.event_tag;
            // Publish the payload above to the consumer's acquire-load of seq.
            slot.seq.store(tail + 1, std::memory_order_release);
            tail_.store(tail + 1, std::memory_order_relaxed);
            return true;
        }

        [[nodiscard]] bool pop(T &out) noexcept
        {
            const std::uint64_t head = head_.load(std::memory_order_relaxed);
            const Slot &slot = buffer_[head & kMask];
            if (slot.seq.load(std::memory_order_acquire) != head + 1)
            {
                return false;
            }

            out = T{};
            out.order_id = slot.order_id;
            out.price = slot.price;
            out.timestamp_ns = slot.timestamp_ns;
            out.client_id = slot.client_id;
            out.qty = slot.qty;
            out.side = slot.side;
            out.tif = slot.tif;
            out.event_tag = slot.event_tag;
            // Publish the freed slot after its payload has been copied out.
            head_.store(head + 1, std::memory_order_release);
            return true;
        }
    };

} // namespace hydra
