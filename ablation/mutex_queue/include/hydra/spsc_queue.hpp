#pragma once

// ablation/mutex_queue/include/hydra/spsc_queue.hpp
//
// Rejected-alternative ablation for the lock-free SPSC ring buffer
// (include/hydra/spsc_queue.hpp): the same push()/pop()/capacity()/
// approx_size() public contract, backed by a std::mutex + std::queue<T>
// instead of the real header's atomic head_/tail_ ring.
//
// WHY this file exists instead of #ifdef-ing the real spsc_queue.hpp: this
// ablation is never compiled into the real build. It is picked up only by
// the blitz_lob_ablation_mutex_queue CMake target, whose
// target_include_directories lists this file's directory BEFORE the real
// include/ -- the compiler's quote-include search then resolves
// #include "hydra/spsc_queue.hpp" (used unmodified by pipeline.hpp) to this
// file instead, and falls through to the real include/ for every other
// header. Production code is never touched, never has a mutex-queue code
// path lurking behind a flag that could accidentally ship.
//
// See docs/DESIGN.md's "Lock-free SPSC queue" section and
// scripts/run_ablations.sh for how this gets built and measured.

#include <cstddef>
#include <mutex>
#include <queue>
#include <type_traits>

namespace hydra
{

    template <typename T, std::size_t N>
    class SpscQueue
    {
        static_assert((N & (N - 1)) == 0, "N must be power of 2");
        static_assert(std::is_trivially_copyable_v<T>,
                      "SpscQueue moves T across threads by value; T must be "
                      "trivially copyable so assignment is a safe, tearing-free "
                      "cross-thread handoff");

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
