#pragma once

#include <cstddef>
#include <cstdint>

// ENVIRONMENT_REQUIREMENTS: isolated RX/matching cores, performance governor,
// no SMT sibling on either core, and memory bound to one NUMA node.
// See ALL_COMMANDS.md for setup and METHODOLOGY.md for measurement requirements.
// Verify CPU topology before reusing the host-specific defaults below.

namespace hydra
{

    // Default RX and matching CPUs. Choose distinct physical cores and keep
    // the kernel isolation settings aligned with these IDs. Verify with
    // /sys/devices/system/cpu/cpu*/topology/thread_siblings_list.
    constexpr int RX_CORE_ID = 4;
    constexpr int MATCHING_CORE_ID = 5;

    // Power-of-two capacity permits masked ring indexing.
    constexpr size_t SPSC_CAPACITY = 65536;
    static_assert((SPSC_CAPACITY & (SPSC_CAPACITY - 1)) == 0, "SPSC_CAPACITY must be a power of 2");

    // Pool exhaustion is reported by acquire() returning nullptr.
    // Fill notifications are synchronous and require no pool.
    constexpr size_t ORDER_POOL_SIZE = 65536;
    constexpr size_t LEVEL_POOL_SIZE = 4096;

    // Warm up before measurement and aggregate independent replay trials.
    constexpr size_t WARMUP_ITERATIONS = 10'000;
    constexpr size_t MEASURED_ITERATIONS = 100'000;
    constexpr size_t DEFAULT_TRIAL_COUNT = 5;

    // Dataset reproducibility requires the seed and full configuration.
    // See datasets/manifest.txt for committed dataset configurations.
    constexpr uint64_t DEFAULT_DATASET_SEED = 42;

    // One dataset contains the warmup and measured windows.
    constexpr size_t DEFAULT_DATASET_ORDER_COUNT =
        WARMUP_ITERATIONS + MEASURED_ITERATIONS; // 110,000

    constexpr int64_t DEFAULT_MID_PRICE = 100'000;
    constexpr int64_t DEFAULT_PRICE_SPREAD_TICKS = 500;
    constexpr uint32_t DEFAULT_MIN_QTY = 1;
    constexpr uint32_t DEFAULT_MAX_QTY = 1000;

    // Exercise cancellation while retaining resting liquidity.
    constexpr double DEFAULT_CANCEL_RATIO = 0.15;
    constexpr double DEFAULT_ARRIVAL_RATE_HZ = 100'000.0;

    // Exercise IOC and FOK while keeping GTC as the dominant order type.
    constexpr double DEFAULT_IOC_RATIO = 0.05;
    constexpr double DEFAULT_FOK_RATIO = 0.02;
    // Remainder (1 - 0.05 - 0.02 = 0.93) is GTC.

} // namespace hydra
