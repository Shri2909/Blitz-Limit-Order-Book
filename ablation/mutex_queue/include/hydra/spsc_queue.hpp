#pragma once

// Queue ablation: same SpscQueue API backed by std::mutex and std::queue.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <type_traits>

#include "hydra/types.hpp"

namespace hydra
{

    template <typename T, std::size_t N> class SpscQueue
    {
        static_assert((N & (N - 1)) == 0, "N must be power of 2");
        static_assert(std::is_trivially_copyable_v<T>,
                      "SpscQueue hands T across threads by value; T must be trivially copyable");

    public:
        SpscQueue() = default;
        SpscQueue(const SpscQueue &) = delete;
        SpscQueue &operator=(const SpscQueue &) = delete;

        static constexpr std::size_t capacity() noexcept { return N; }

        std::size_t approx_size() const noexcept
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.size();
        }

        [[nodiscard]] bool push(const T &item) noexcept
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.size() >= N)
                {
                    return false;
                }
                queue_.push(item);
            }
            return true;
        }

        [[nodiscard]] bool pop(T &out) noexcept
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty())
            {
                return false;
            }
            out = queue_.front();
            queue_.pop();
            return true;
        }

    private:
        mutable std::mutex mutex_;
        std::queue<T> queue_;
    };

    // Order specialization carrying the same payload as the production one-line slot (see
    // include/hydra/spsc_queue.hpp: order_id, price, qty, side, tif, event_tag, client_id,
    // timestamp_ns), so this ablation isolates the synchronisation scheme (mutex + std::queue
    // against the lock-free ring) and not how many bytes are copied per event.
    template <typename T, std::size_t N>
        requires(std::is_same_v<T, Order>)
    class SpscQueue<T, N>
    {
        static_assert((N & (N - 1)) == 0, "N must be power of 2");

        struct Payload
        {
            std::uint64_t order_id;
            std::int64_t price;
            std::uint64_t timestamp_ns;
            std::uint64_t client_id;
            std::uint32_t qty;
            Side side;
            TimeInForce tif;
            OrderEventTag event_tag;
        };

    public:
        SpscQueue() = default;
        SpscQueue(const SpscQueue &) = delete;
        SpscQueue &operator=(const SpscQueue &) = delete;

        static constexpr std::size_t capacity() noexcept { return N; }

        std::size_t approx_size() const noexcept
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.size();
        }

        [[nodiscard]] bool push(const T &item) noexcept
        {
            const Payload payload{item.order_id, item.price, item.timestamp_ns, item.client_id,
                                  item.qty,      item.side,  item.tif,          item.event_tag};
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.size() >= N)
            {
                return false;
            }
            queue_.push(payload);
            return true;
        }

        [[nodiscard]] bool pop(T &out) noexcept
        {
            Payload payload;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty())
                {
                    return false;
                }
                payload = queue_.front();
                queue_.pop();
            }
            out = T{};
            out.order_id = payload.order_id;
            out.price = payload.price;
            out.timestamp_ns = payload.timestamp_ns;
            out.client_id = payload.client_id;
            out.qty = payload.qty;
            out.side = payload.side;
            out.tif = payload.tif;
            out.event_tag = payload.event_tag;
            return true;
        }

    private:
        mutable std::mutex mutex_;
        std::queue<Payload> queue_;
    };

} // namespace hydra
