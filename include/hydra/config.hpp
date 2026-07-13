#pragma once

#include <cstddef>
#include <cstdint>

// ============================================================================
// ENVIRONMENT_REQUIREMENTS
// ============================================================================
// The following kernel and hardware configuration is REQUIRED for the latency
// figures this system targets. Deviating from any of these will produce
// meaningless benchmark numbers.
//
// 1. GRUB kernel parameters (add to GRUB_CMDLINE_LINUX in /etc/default/grub,
//    then run: sudo update-grub && sudo reboot):
//        isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
//    WHY: isolcpus removes cores 2 and 3 from the kernel scheduler pool so
//    no unrelated task ever preempts the RX or matching threads. nohz_full
//    suppresses the periodic timer interrupt on those cores. rcu_nocbs offloads
//    RCU callbacks off the isolated cores.
//
// 2. CPU frequency governor (can be applied live — no reboot needed):
//        sudo cpupower frequency-set --governor performance
//    WHY: On-demand/powersave governors introduce P-state transitions mid-run,
//    adding hundreds of ns of jitter to latency measurements.
//
// 3. Simultaneous Multi-Threading (SMT / Hyper-Threading) must be OFF:
//        echo off | sudo tee /sys/devices/system/cpu/smt/control
//    WHY: An SMT sibling on the same physical core shares execution resources
//    (L1i, decoders, execution ports) with the hot-path thread, adding
//    unpredictable latency spikes.
//
// 4. NUMA: Run the binary bound to a single NUMA node:
//        numactl --cpunodebind=0 --membind=0 ./build/blitz_lob --benchmark ...
//    WHY: Cross-node memory access costs ~2-3x local-node latency on typical
//    dual-socket servers. Even a single stray remote access to a pool slab or
//    queue buffer can inflate tail latency dramatically.
// ============================================================================

namespace hydra {

// ── Core affinity ────────────────────────────────────────────────────────────
// WHY: Hard-coded to specific physical cores that match the isolcpus= kernel
// parameter above. Changing these without also updating the boot parameter
// defeats the isolation.
constexpr int RX_CORE_ID       = 2;
constexpr int MATCHING_CORE_ID = 3;

// ── SPSC queue capacity ──────────────────────────────────────────────────────
// WHY: Must be a power of 2 so slot lookup reduces to a single bitmask
// operation (index & (N-1)) with no division or branch.
constexpr size_t SPSC_CAPACITY = 65536;
static_assert(
    (SPSC_CAPACITY & (SPSC_CAPACITY - 1)) == 0,
    "SPSC_CAPACITY must be a power of 2"
);

// ── Object pool sizes ────────────────────────────────────────────────────────
// WHY: Sized to comfortably exceed the maximum number of simultaneously resting
// orders/levels observed in realistic market-data replay. Exhaustion is a
// recoverable condition (acquire() returns nullptr) not a crash.
constexpr size_t ORDER_POOL_SIZE      = 65536;
constexpr size_t LEVEL_POOL_SIZE      = 4096;
constexpr size_t FILL_EVENT_POOL_SIZE = 10240;

// ── Benchmark parameters ─────────────────────────────────────────────────────
// WHY: 10k warmup iterations allow the branch predictor, instruction cache,
// and TLB to reach steady state before measurements begin. 100k measured
// iterations gives enough samples for stable P99/P99.9 estimates. 5 trials
// allows median-across-trials to suppress single-run outliers.
constexpr size_t WARMUP_ITERATIONS   = 10'000;
constexpr size_t MEASURED_ITERATIONS = 100'000;
constexpr size_t DEFAULT_TRIAL_COUNT = 5;

// ── Dataset generation defaults ──────────────────────────────────────────────
// These are the fallback values used by DatasetGenerator and blitz_gen_dataset
// when a flag/field is omitted. Document any dataset referenced in
// docs/DESIGN.md by its full config (not just the seed) — see
// datasets/manifest.txt.
//
// WHY seed=42: arbitrary but documented. The reproducibility contract is
// seed + full config, not seed alone; the same seed with different order_count
// or cancel_ratio produces a different file.
constexpr uint64_t DEFAULT_DATASET_SEED = 42;

// WHY: Sized to map directly onto WARMUP_ITERATIONS + MEASURED_ITERATIONS so
// benchmark.cpp's replay mode consumes the file without special-casing the
// warmup/measurement split.
constexpr size_t DEFAULT_DATASET_ORDER_COUNT =
    WARMUP_ITERATIONS + MEASURED_ITERATIONS;  // 110,000

constexpr int64_t  DEFAULT_MID_PRICE          = 100'000;
constexpr int64_t  DEFAULT_PRICE_SPREAD_TICKS = 500;
constexpr uint32_t DEFAULT_MIN_QTY            = 1;
constexpr uint32_t DEFAULT_MAX_QTY            = 1000;

// WHY: 15% cancel ratio reflects realistic market microstructure — most HFT
// venues see cancel/new-order ratios well above 10:1, but for a synthetic
// benchmark a 0.15 rate exercises the cancel path without emptying the book.
constexpr double DEFAULT_CANCEL_RATIO    = 0.15;
constexpr double DEFAULT_ARRIVAL_RATE_HZ = 100'000.0;

// WHY: IOC and FOK are the minority TIF cases in realistic order flow; most
// orders are GTC. These small defaults exercise those paths in benchmark runs
// without making them the dominant case, which would distort latency averages
// away from the GTC hot path.
constexpr double DEFAULT_IOC_RATIO = 0.05;
constexpr double DEFAULT_FOK_RATIO = 0.02;
// Remainder (1 - 0.05 - 0.02 = 0.93) is GTC.

} // namespace hydra
