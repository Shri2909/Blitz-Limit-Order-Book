#pragma once

// Queue ablation: same SpscQueue API backed by std::mutex and std::queue.

#include <cstddef>
#include <mutex>
#include <queue>
#include <type_traits>

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

} // namespace hydra
