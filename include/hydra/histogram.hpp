#pragma once

// include/hydra/histogram.hpp
//
// Lock-free, O(1)-record latency histogram with percentile query, safe to
// read (query_percentile()) while concurrently being written (record()).

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <thread> // std::this_thread::yield -- control path only, never record()

namespace hydra
{

    class HdrHistogram
    {
    public:
        HdrHistogram() = default;

        // Coordinated omission (Gil Tene): if the sender is not fixed-rate,
        // stalls create gaps that never get represented in the latency
        // distribution, understating tail latency. This is precisely why
        // benchmark.cpp's synthetic/replay sender must be fixed-rate rather
        // than send-as-fast-as-possible -- a histogram is only as honest as
        // the arrival process feeding it.
        void record(uint64_t latency_ns) noexcept
        {
            const auto idx = static_cast<std::size_t>(live_.load(std::memory_order_acquire));
            // Mark this buffer as having an in-flight writer *before* the
            // fetch_add, so swap_buffers() can detect and wait out any
            // writer that read `live_` just before a flip. This is the
            // lock-free replacement for a mutex: two extra uncontended
            // atomic RMW ops on the hot path, never a lock.
            in_flight_[idx].fetch_add(1, std::memory_order_acquire);
            buffers_[idx][bucket_index(latency_ns)].fetch_add(1, std::memory_order_relaxed);
            in_flight_[idx].fetch_sub(1, std::memory_order_release);
        }

        // Off-hot-path control operation -- spins briefly instead of
        // locking, so this header never needs <mutex>. Never called from
        // matching_thread_fn's per-order loop. swap_buffers() zeroes out the
        // buffer that is *currently* cold (about to become the new
        // live/write target) and query_percentile() reads that same cold
        // buffer, so the two must never observe each other's buffer
        // mid-mutation -- the readers_/in_flight_ spin-waits below are what
        // enforce that without a lock.
        void swap_buffers() noexcept
        {
            const int old_live = live_.load(std::memory_order_relaxed);
            const int new_live = 1 - old_live;
            const auto new_idx = static_cast<std::size_t>(new_live);
            const auto old_idx = static_cast<std::size_t>(old_live);

            // new_idx is the buffer query_percentile() has been reading.
            // Don't start zeroing it until any in-flight reader is done.
            while (readers_[new_idx].load(std::memory_order_acquire) != 0)
            {
                std::this_thread::yield();
            }

            for (auto &bucket : buffers_[new_idx])
            {
                bucket.store(0, std::memory_order_relaxed);
            }

            live_.store(new_live, std::memory_order_release);

            // A record() call that already read the OLD live_ value may
            // still be mid-flight into buffers_[old_idx]. Wait for it so the
            // next query_percentile() (which will read buffers_[old_idx] as
            // the new cold buffer) sees a complete, untorn snapshot.
            while (in_flight_[old_idx].load(std::memory_order_acquire) != 0)
            {
                std::this_thread::yield();
            }
        }

        [[nodiscard]] double query_percentile(double p) const noexcept
        {
            const auto cold_idx =
                static_cast<std::size_t>(1 - live_.load(std::memory_order_acquire));
            readers_[cold_idx].fetch_add(1, std::memory_order_acquire);

            const auto &buf = buffers_[cold_idx];

            uint64_t total = 0;
            for (const auto &bucket : buf)
            {
                total += bucket.load(std::memory_order_relaxed);
            }

            double result = static_cast<double>(bucket_lower_bound(kNumBuckets - 1));
            if (total != 0)
            {
                const double target_rank = (p / 100.0) * static_cast<double>(total);

                uint64_t cumulative = 0;
                for (std::size_t b = 0; b < kNumBuckets; ++b)
                {
                    const uint64_t count = buf[b].load(std::memory_order_relaxed);
                    if (count == 0)
                    {
                        continue;
                    }
                    const uint64_t next_cumulative = cumulative + count;
                    if (static_cast<double>(next_cumulative) >= target_rank)
                    {
                        const double lower = static_cast<double>(bucket_lower_bound(b));
                        const double width = static_cast<double>(bucket_width(b));
                        const double fraction =
                            (target_rank - static_cast<double>(cumulative)) / static_cast<double>(count);
                        result = lower + fraction * width;
                        break;
                    }
                    cumulative = next_cumulative;
                }
            }
            else
            {
                result = 0.0;
            }

            readers_[cold_idx].fetch_sub(1, std::memory_order_release);
            return result;
        }

    private:
        static constexpr unsigned kSubBucketBits = 5;
        static constexpr uint64_t kSubBucketCount = uint64_t{1} << kSubBucketBits;
        static constexpr unsigned kNumBinades = 64 - kSubBucketBits;
        static constexpr std::size_t kNumBuckets =
            static_cast<std::size_t>(kSubBucketCount) * (1 + kNumBinades);

        static_assert(std::atomic<uint64_t>::is_always_lock_free,
                      "HdrHistogram requires std::atomic<uint64_t> to be lock-free on this "
                      "target; a mutex-emulated atomic would silently break the wait-free, "
                      "hot-path-safe contract record() promises.");
        static_assert(std::atomic<uint32_t>::is_always_lock_free,
                      "in_flight_/readers_ must be lock-free too, or record() would inherit "
                      "a hidden lock via a mutex-emulated std::atomic<uint32_t>.");

        std::array<std::array<std::atomic<uint64_t>, kNumBuckets>, 2> buffers_{};
        std::atomic<int> live_{0};
        std::array<std::atomic<uint32_t>, 2> in_flight_{};
        mutable std::array<std::atomic<uint32_t>, 2> readers_{};

        [[nodiscard]] static std::size_t bucket_index(uint64_t latency_ns) noexcept
        {
            if (latency_ns < kSubBucketCount)
            {
                return static_cast<std::size_t>(latency_ns);
            }
            const unsigned msb = 63U - static_cast<unsigned>(std::countl_zero(latency_ns));
            const unsigned binade = msb - kSubBucketBits;
            const uint64_t sub_bucket = (latency_ns >> binade) & (kSubBucketCount - 1);
            return static_cast<std::size_t>(kSubBucketCount) +
                   static_cast<std::size_t>(binade) * static_cast<std::size_t>(kSubBucketCount) +
                   static_cast<std::size_t>(sub_bucket);
        }

        [[nodiscard]] static uint64_t bucket_lower_bound(std::size_t bucket) noexcept
        {
            if (bucket < kSubBucketCount)
            {
                return bucket;
            }
            const std::size_t idx = bucket - kSubBucketCount;
            const unsigned binade = static_cast<unsigned>(idx >> kSubBucketBits);
            const uint64_t sub = static_cast<uint64_t>(idx) & (kSubBucketCount - 1);
            return (kSubBucketCount + sub) << binade;
        }

        [[nodiscard]] static uint64_t bucket_width(std::size_t bucket) noexcept
        {
            if (bucket < kSubBucketCount)
            {
                return 1;
            }
            const std::size_t idx = bucket - kSubBucketCount;
            const unsigned binade = static_cast<unsigned>(idx >> kSubBucketBits);
            return uint64_t{1} << binade;
        }
    };

} // namespace hydra
