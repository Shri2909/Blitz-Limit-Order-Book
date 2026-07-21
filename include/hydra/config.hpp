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
//        isolcpus=4,5 nohz_full=4,5 rcu_nocbs=4,5
//    WHY: isolcpus removes cores 4 and 5 from the kernel scheduler pool so
//    no unrelated task ever preempts the RX or matching threads. nohz_full
//    suppresses the periodic timer interrupt on those cores. rcu_nocbs offloads
//    RCU callbacks off the isolated cores.
//    NOTE: 4,5 are specific to this host's CPU topology (see the Core affinity
//    section below) -- verify with thread_siblings_list before reusing on
//    different hardware; do not copy these two numbers blindly.
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

namespace hydra
{

    // ── Core affinity ────────────────────────────────────────────────────────────
    // WHY: Hard-coded to specific physical cores that match the isolcpus= kernel
    // parameter above. Changing these without also updating the boot parameter
    // defeats the isolation.
    // NOTE: 4/5 chosen instead of 2/3 because on this host's CPU (12th-gen
    // Intel hybrid: P-cores 0-1 have SMT siblings {0,1} and {2,3}; E-cores
    // 4+ have no SMT sibling at all) cores 2 and 3 are the two hyperthreads
    // of the SAME physical P-core -- check_smt_off() in benchmark.cpp can
    // never pass for that pair, since disabling SMT machine-wide takes core
    // 3 offline entirely rather than freeing it. Verify with:
    //   cat /sys/devices/system/cpu/cpu*/topology/thread_siblings_list
    // before reusing this constant on different hardware.
    constexpr int RX_CORE_ID = 4;
    constexpr int MATCHING_CORE_ID = 5;

    // ── SPSC queue capacity ──────────────────────────────────────────────────────
    // WHY: Must be a power of 2 so slot lookup reduces to a single bitmask
    // operation (index & (N-1)) with no division or branch.
    constexpr size_t SPSC_CAPACITY = 65536;
    static_assert(
        (SPSC_CAPACITY & (SPSC_CAPACITY - 1)) == 0,
        "SPSC_CAPACITY must be a power of 2");

    // ── Object pool sizes ────────────────────────────────────────────────────────
    // WHY: Sized to comfortably exceed the maximum number of simultaneously resting
    // orders/levels observed in realistic market-data replay. Exhaustion is a
    // recoverable condition (acquire() returns nullptr) not a crash.
    // NOTE: no FillEvent pool -- FillEvent notifications are delivered
    // synchronously and never outlive Matcher::apply_fill(), so there is
    // nothing for a pool to buy here (see matcher.hpp's apply_fill()).
    constexpr size_t ORDER_POOL_SIZE = 65536;
    constexpr size_t LEVEL_POOL_SIZE = 4096;

    // ── Benchmark parameters ─────────────────────────────────────────────────────
    // WHY: 10k warmup iterations allow the branch predictor, instruction cache,
    // and TLB to reach steady state before measurements begin. 100k measured
    // iterations gives enough samples for stable P99/P99.9 estimates. 5 trials
    // allows median-across-trials to suppress single-run outliers.
    constexpr size_t WARMUP_ITERATIONS = 10'000;
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
        WARMUP_ITERATIONS + MEASURED_ITERATIONS; // 110,000

    constexpr int64_t DEFAULT_MID_PRICE = 100'000;
    constexpr int64_t DEFAULT_PRICE_SPREAD_TICKS = 500;
    constexpr uint32_t DEFAULT_MIN_QTY = 1;
    constexpr uint32_t DEFAULT_MAX_QTY = 1000;

    // WHY: 15% cancel ratio reflects realistic market microstructure — most HFT
    // venues see cancel/new-order ratios well above 10:1, but for a synthetic
    // benchmark a 0.15 rate exercises the cancel path without emptying the book.
    constexpr double DEFAULT_CANCEL_RATIO = 0.15;
    constexpr double DEFAULT_ARRIVAL_RATE_HZ = 100'000.0;

    // WHY: IOC and FOK are the minority TIF cases in realistic order flow; most
    // orders are GTC. These small defaults exercise those paths in benchmark runs
    // without making them the dominant case, which would distort latency averages
    // away from the GTC hot path.
    constexpr double DEFAULT_IOC_RATIO = 0.05;
    constexpr double DEFAULT_FOK_RATIO = 0.02;
    // Remainder (1 - 0.05 - 0.02 = 0.93) is GTC.

    // ── AF_XDP (Phase 10) ────────────────────────────────────────────────────────
    // Scoped to include/hydra/xdp/{xdp_socket,zero_copy_parser}.hpp and
    // net/xdp_prog.bpf.c; only compiled when ENABLE_AFXDP is ON (see
    // CMakeLists.txt). Grouped in its own `namespace hydra::config` block
    // rather than the flat `namespace hydra` above, matching how
    // xdp_socket.hpp/zero_copy_parser.hpp reference these constants
    // (hydra::config::AFXDP_*) -- this is deliberate, not an inconsistency:
    // it keeps every AF_XDP tunable under one qualified prefix instead of
    // widening the flat hydra:: namespace with names that only make sense
    // together.
    namespace config
    {

        // Number of UMEM frames pre-allocated for one XdpSocket's
        // free-frame pool. Power-of-2 so ring index math stays a bitmask,
        // consistent with SPSC_CAPACITY above.
        constexpr size_t AFXDP_NUM_FRAMES = 4096;
        static_assert((AFXDP_NUM_FRAMES & (AFXDP_NUM_FRAMES - 1)) == 0,
                      "AFXDP_NUM_FRAMES must be a power of 2");

        // Must equal XSK_UMEM__DEFAULT_FRAME_SIZE from <xdp/xsk.h>
        // (currently 4096, i.e. 1 << XSK_UMEM__DEFAULT_FRAME_SHIFT).
        // Checked with a static_assert against the real libbpf/libxdp
        // constant inside net/xdp_socket_user.cpp, so the two can never
        // silently drift apart.
        constexpr size_t AFXDP_FRAME_SIZE = 4096;

        constexpr size_t AFXDP_UMEM_SIZE = AFXDP_NUM_FRAMES * AFXDP_FRAME_SIZE;

        // Max frames drained from the RX ring in one
        // XdpSocket::recv_batch() call.
        constexpr size_t AFXDP_RX_BATCH_SIZE = 64;

        // Default {netdev, queue} binding used by tooling when the caller
        // doesn't override it.
        constexpr uint32_t AFXDP_DEFAULT_QUEUE_ID = 0;

        // UDP port the order-entry wire protocol uses; net/xdp_prog.bpf.c
        // redirects UDP traffic on the bound queue regardless of port
        // (port filtering is left to the matching pipeline), but tooling
        // needs one concrete default to talk to.
        constexpr uint16_t AFXDP_ORDER_ENTRY_UDP_PORT = 40000;

    } // namespace config

} // namespace hydra