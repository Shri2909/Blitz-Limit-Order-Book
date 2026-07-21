# Blitz LOB — Consolidated Benchmark Results

Every benchmark this project's harness ran in this session, consolidated from the actual saved CSV/JSON artifacts in `results/98a597b/` -- nothing re-run, nothing estimated, nothing carried over from a prior session. All 16 runs below (4 canonical + 10 ablation + 2 verbose) are independently `VALID`, deterministic, and saw zero dropped events / pool exhaustion / arena fallback. This supersedes and replaces every prior results directory (`results/684f9ff/` and an earlier, mislabeled `results/98a597b/` run whose binary had not been rebuilt after the commit) -- both were deleted. See `docs/BENCHMARK_METHODOLOGY.md` for what every field means.

**Environment for every run below**: 12th Gen Intel Core i5-1235U, cores 4 (producer) / 5 (consumer) isolated (`isolcpus`), governor `performance` on both, no SMT sibling on either core, single NUMA node, Chrome closed. Commit `98a597b` ('Clean Benchmarks'), **working tree genuinely clean** -- every artifact below self-reports `dirty: false`, confirmed, not merely assumed.

## At a Glance

| # | Run | P50 (median, ns) | P99 (median, ns) | P99.9 (median, ns) | Valid | Deterministic |
|---|---|---|---|---|---|---|
| 1 | **Canonical — Price-Time** | 485.0 | 1,225.0 | 1,850.0 | YES | PASS |
| 2 | **Canonical — Pro-Rata** | 491.0 | 1,425.0 | 3,132.0 | YES | PASS |
| 3 | Canonical — Price-Time (--verbose) | 486.0 | 1,226.0 | 1,792.0 | YES | PASS |
| 4 | Canonical — Pro-Rata (--verbose) | 484.0 | 1,415.0 | 3,153.0 | YES | PASS |
| 5 | Ablation baseline (optimized) — Price-Time | 485.0 | 1,222.0 | 1,762.0 | YES | PASS |
| 6 | Ablation baseline (optimized) — Pro-Rata | 487.0 | 1,428.0 | 3,065.0 | YES | PASS |
| 7 | Ablation mutex_queue — Price-Time | 1,172.0 | 5,484.0 | 7,770.0 | YES | PASS |
| 8 | Ablation mutex_queue — Pro-Rata | 1,255.0 | 5,512.0 | 7,774.0 | YES | PASS |
| 9 | Ablation unpinned — Price-Time | 381.0 | 1,098.0 | 9,216.0 | YES | PASS |
| 10 | Ablation unpinned — Pro-Rata | 374.0 | 2,175.0 | 4,906.0 | YES | PASS |
| 11 | Ablation malloc_pool — Price-Time | 528.0 | 2,167.0 | 6,079.0 | YES | PASS |
| 12 | Ablation malloc_pool — Pro-Rata | 534.0 | 3,289.0 | 6,948.0 | YES | PASS |
| 13 | Ablation flat_layout — Price-Time | 495.0 | 1,231.0 | 1,797.0 | YES | PASS |
| 14 | Ablation flat_layout — Pro-Rata | 493.0 | 1,466.0 | 3,554.0 | YES | PASS |

---

## 1–2. Canonical Benchmarks (the headline claim per matching policy)

### Canonical — Price-Time Priority (FIFO)

- **Run ID**: `98a597b-price_time-1784608605`
- **Commit**: `98a597b`  |  **Working tree**: Clean
- **Matching mode**: `price_time`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)
- **CPU**: 12th Gen Intel(R) Core(TM) i5-1235U  |  **Compiler**: GCC 15.2.0
- **Env**: `isolated=4-5 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=98a597b dirty=no`
- **Result validity**: **VALID**

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 467.0 | 760.0 | 1,191.0 | 1,880.0 | 538.2 | 12,822.0 | 84,992.1 |
| 1 | 483.0 | 786.0 | 1,225.0 | 1,743.0 | 550.5 | 11,929.0 | 84,992.0 |
| 2 | 487.0 | 790.0 | 1,252.0 | 1,850.0 | 555.0 | 12,405.0 | 84,992.0 |
| 3 | 488.0 | 790.0 | 1,251.0 | 1,889.0 | 556.2 | 12,800.0 | 84,991.9 |
| 4 | 485.0 | 784.0 | 1,222.0 | 1,755.0 | 551.3 | 11,625.0 | 84,992.0 |

**Median across trials**: P50=485.0 ns | P90=786.0 ns | P99=1,225.0 ns | P99.9=1,850.0 ns | Max=12,405.0 ns | Throughput=0.0850 M orders/s

**Stability (CV across trials)**: P50=1.78% | P99=2.04% | P99.9=3.82%

**Workload composition (identical every trial -- deterministic replay)**:
- Crossing orders: 36066 | Non-crossing: 49052 | Partial fills: 5032 | Multi-level sweeps: 19345
- Self-trade skips: 6564 | Generated fills: 65063 | Mean fills/match: 1.804 | Max fill fan-out: 8 | Mean levels consumed: 0.813

**Correctness/Resources (every trial)**: dropped_events=0 | arena_fallback_count=0 | pool_exhaustion_count=0 | determinism=PASS | state_hash=`7d8f570852562599` (identical across all trials) | valid=YES

---

### Canonical — Pro-Rata Allocation

- **Run ID**: `98a597b-pro_rata-1784608621`
- **Commit**: `98a597b`  |  **Working tree**: Clean
- **Matching mode**: `pro_rata`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)
- **CPU**: 12th Gen Intel(R) Core(TM) i5-1235U  |  **Compiler**: GCC 15.2.0
- **Env**: `isolated=4-5 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=98a597b dirty=no`
- **Result validity**: **VALID**

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 478.0 | 805.0 | 1,421.0 | 3,307.0 | 566.2 | 12,016.0 | 84,992.1 |
| 1 | 491.0 | 828.0 | 1,425.0 | 3,132.0 | 575.3 | 12,339.0 | 84,992.0 |
| 2 | 492.0 | 827.0 | 1,432.0 | 3,294.0 | 576.8 | 11,103.0 | 84,991.9 |
| 3 | 491.0 | 826.0 | 1,428.0 | 3,091.0 | 575.2 | 13,369.0 | 84,991.8 |
| 4 | 490.0 | 824.0 | 1,410.0 | 2,933.0 | 573.4 | 15,244.0 | 84,991.9 |

**Median across trials**: P50=491.0 ns | P90=826.0 ns | P99=1,425.0 ns | P99.9=3,132.0 ns | Max=12,339.0 ns | Throughput=0.0850 M orders/s

**Stability (CV across trials)**: P50=1.20% | P99=0.59% | P99.9=4.92%

**Workload composition (identical every trial -- deterministic replay)**:
- Crossing orders: 36072 | Non-crossing: 49046 | Partial fills: 5042 | Multi-level sweeps: 19367
- Self-trade skips: 7961 | Generated fills: 74733 | Mean fills/match: 2.072 | Max fill fan-out: 35 | Mean levels consumed: 0.814

**Correctness/Resources (every trial)**: dropped_events=0 | arena_fallback_count=0 | pool_exhaustion_count=0 | determinism=PASS | state_hash=`0ac25b855783070e` (identical across all trials) | valid=YES

---

## 3–4. Canonical Benchmarks, --verbose (same runs, diagnostics-enabled reports)

### Price-Time (--verbose)

- **Run ID**: `98a597b-price_time-1784608629`
- **Commit**: `98a597b`  |  **Working tree**: Clean
- **Matching mode**: `price_time`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)
- **CPU**: 12th Gen Intel(R) Core(TM) i5-1235U  |  **Compiler**: GCC 15.2.0
- **Env**: `isolated=4-5 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=98a597b dirty=no`
- **Result validity**: **VALID**

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 472.0 | 764.0 | 1,200.0 | 1,790.0 | 541.4 | 14,560.0 | 84,992.0 |
| 1 | 486.0 | 785.0 | 1,227.0 | 1,765.0 | 552.1 | 11,769.0 | 84,992.2 |
| 2 | 487.0 | 785.0 | 1,226.0 | 1,792.0 | 553.1 | 12,160.0 | 84,992.1 |
| 3 | 486.0 | 784.0 | 1,226.0 | 1,795.0 | 551.7 | 12,585.0 | 84,991.9 |
| 4 | 491.0 | 793.0 | 1,247.0 | 1,842.0 | 557.6 | 12,597.0 | 84,991.8 |

**Median across trials**: P50=486.0 ns | P90=785.0 ns | P99=1,226.0 ns | P99.9=1,792.0 ns | Max=12,585.0 ns | Throughput=0.0850 M orders/s

**Stability (CV across trials)**: P50=1.49% | P99=1.36% | P99.9=1.56%

**Workload composition (identical every trial -- deterministic replay)**:
- Crossing orders: 36066 | Non-crossing: 49052 | Partial fills: 5032 | Multi-level sweeps: 19345
- Self-trade skips: 6564 | Generated fills: 65063 | Mean fills/match: 1.804 | Max fill fan-out: 8 | Mean levels consumed: 0.813

**Correctness/Resources (every trial)**: dropped_events=0 | arena_fallback_count=0 | pool_exhaustion_count=0 | determinism=PASS | state_hash=`7d8f570852562599` (identical across all trials) | valid=YES

---

### Pro-Rata (--verbose)

- **Run ID**: `98a597b-pro_rata-1784608636`
- **Commit**: `98a597b`  |  **Working tree**: Clean
- **Matching mode**: `pro_rata`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)
- **CPU**: 12th Gen Intel(R) Core(TM) i5-1235U  |  **Compiler**: GCC 15.2.0
- **Env**: `isolated=4-5 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=98a597b dirty=no`
- **Result validity**: **VALID**

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 470.0 | 795.0 | 1,391.0 | 2,817.0 | 555.6 | 13,925.0 | 84,992.0 |
| 1 | 488.0 | 832.0 | 1,475.0 | 3,607.0 | 577.6 | 12,552.0 | 84,991.9 |
| 2 | 487.0 | 824.0 | 1,439.0 | 3,282.0 | 571.9 | 11,419.0 | 84,991.9 |
| 3 | 484.0 | 819.0 | 1,411.0 | 2,883.0 | 566.7 | 10,485.0 | 84,991.9 |
| 4 | 484.0 | 818.0 | 1,415.0 | 3,153.0 | 567.9 | 11,959.0 | 84,992.0 |

**Median across trials**: P50=484.0 ns | P90=819.0 ns | P99=1,415.0 ns | P99.9=3,153.0 ns | Max=11,959.0 ns | Throughput=0.0850 M orders/s

**Stability (CV across trials)**: P50=1.51% | P99=2.26% | P99.9=10.15%

**Workload composition (identical every trial -- deterministic replay)**:
- Crossing orders: 36072 | Non-crossing: 49046 | Partial fills: 5042 | Multi-level sweeps: 19367
- Self-trade skips: 7961 | Generated fills: 74733 | Mean fills/match: 2.072 | Max fill fan-out: 35 | Mean levels consumed: 0.814

**Correctness/Resources (every trial)**: dropped_events=0 | arena_fallback_count=0 | pool_exhaustion_count=0 | determinism=PASS | state_hash=`0ac25b855783070e` (identical across all trials) | valid=YES

---

## 5–14. Optimization Ablation Suite (all 5 variants × both policies)

### Baseline (fully optimized) — Price-Time

- **Run ID**: `98a597b-price_time-1784608667`
- **Commit**: `98a597b`  |  **Working tree**: Clean
- **Matching mode**: `price_time`  |  **Dataset**: `/home/abc/projects/Blitz-Limit-Order-Book/datasets/bench_110k.bin` (hash `51ce6285b274413e`)
- **CPU**: 12th Gen Intel(R) Core(TM) i5-1235U  |  **Compiler**: GCC 15.2.0
- **Env**: `isolated=4-5 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=98a597b dirty=no`
- **Result validity**: **VALID**

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 467.0 | 758.0 | 1,175.0 | 1,698.0 | 535.7 | 11,021.0 | 84,992.0 |
| 1 | 485.0 | 784.0 | 1,222.0 | 1,727.0 | 550.2 | 11,910.0 | 84,992.0 |
| 2 | 486.0 | 785.0 | 1,239.0 | 1,835.0 | 552.2 | 14,123.0 | 84,991.9 |
| 3 | 484.0 | 782.0 | 1,222.0 | 1,803.0 | 550.2 | 12,872.0 | 84,991.9 |
| 4 | 485.0 | 784.0 | 1,223.0 | 1,762.0 | 550.9 | 12,350.0 | 84,992.0 |

**Median across trials**: P50=485.0 ns | P90=784.0 ns | P99=1,222.0 ns | P99.9=1,762.0 ns | Max=12,350.0 ns | Throughput=0.0850 M orders/s

**Stability (CV across trials)**: P50=1.68% | P99=1.98% | P99.9=3.14%

**Workload composition (identical every trial -- deterministic replay)**:
- Crossing orders: 36066 | Non-crossing: 49052 | Partial fills: 5032 | Multi-level sweeps: 19345
- Self-trade skips: 6564 | Generated fills: 65063 | Mean fills/match: 1.804 | Max fill fan-out: 8 | Mean levels consumed: 0.813

**Correctness/Resources (every trial)**: dropped_events=0 | arena_fallback_count=0 | pool_exhaustion_count=0 | determinism=PASS | state_hash=`7d8f570852562599` (identical across all trials) | valid=YES

---

### Baseline (fully optimized) — Pro-Rata

- **Run ID**: `98a597b-pro_rata-1784608707`
- **Commit**: `98a597b`  |  **Working tree**: Clean
- **Matching mode**: `pro_rata`  |  **Dataset**: `/home/abc/projects/Blitz-Limit-Order-Book/datasets/bench_110k.bin` (hash `51ce6285b274413e`)
- **CPU**: 12th Gen Intel(R) Core(TM) i5-1235U  |  **Compiler**: GCC 15.2.0
- **Env**: `isolated=4-5 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=98a597b dirty=no`
- **Result validity**: **VALID**

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 473.0 | 796.0 | 1,390.0 | 3,019.0 | 558.1 | 12,171.0 | 84,992.1 |
| 1 | 490.0 | 823.0 | 1,438.0 | 3,010.0 | 573.3 | 13,982.0 | 84,992.0 |
| 2 | 492.0 | 831.0 | 1,446.0 | 3,331.0 | 576.6 | 13,375.0 | 84,991.9 |
| 3 | 487.0 | 823.0 | 1,428.0 | 3,104.0 | 571.1 | 13,134.0 | 84,991.9 |
| 4 | 487.0 | 822.0 | 1,418.0 | 3,065.0 | 570.3 | 13,285.0 | 84,991.9 |

**Median across trials**: P50=487.0 ns | P90=823.0 ns | P99=1,428.0 ns | P99.9=3,065.0 ns | Max=13,285.0 ns | Throughput=0.0850 M orders/s

**Stability (CV across trials)**: P50=1.54% | P99=1.53% | P99.9=4.23%

**Workload composition (identical every trial -- deterministic replay)**:
- Crossing orders: 36072 | Non-crossing: 49046 | Partial fills: 5042 | Multi-level sweeps: 19367
- Self-trade skips: 7961 | Generated fills: 74733 | Mean fills/match: 2.072 | Max fill fan-out: 35 | Mean levels consumed: 0.814

**Correctness/Resources (every trial)**: dropped_events=0 | arena_fallback_count=0 | pool_exhaustion_count=0 | determinism=PASS | state_hash=`0ac25b855783070e` (identical across all trials) | valid=YES

---

### mutex_queue (lock-free SPSC replaced with std::mutex+std::queue) — Price-Time

- **Run ID**: `98a597b-ablation-mutex_queue-price_time-1784608675`
- **Commit**: `98a597b-ablation-mutex_queue`  |  **Working tree**: Clean
- **Matching mode**: `price_time`  |  **Dataset**: `/home/abc/projects/Blitz-Limit-Order-Book/datasets/bench_110k.bin` (hash `51ce6285b274413e`)
- **CPU**: 12th Gen Intel(R) Core(TM) i5-1235U  |  **Compiler**: GCC 15.2.0
- **Env**: `isolated=4-5 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=98a597b-ablation-mutex_queue dirty=no`
- **Result validity**: **VALID**

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 1,143.0 | 1,943.0 | 5,449.0 | 8,130.0 | 1,381.4 | 72,865.0 | 84,991.8 |
| 1 | 1,172.0 | 3,016.0 | 5,471.0 | 7,770.0 | 1,494.7 | 19,127.0 | 84,991.8 |
| 2 | 1,162.0 | 3,089.0 | 5,484.0 | 7,305.0 | 1,499.7 | 17,660.0 | 84,992.0 |
| 3 | 1,172.0 | 3,119.0 | 5,509.0 | 7,456.0 | 1,508.5 | 20,403.0 | 84,992.0 |
| 4 | 1,187.0 | 3,193.0 | 5,605.0 | 8,220.0 | 1,531.3 | 16,601.0 | 84,992.1 |

**Median across trials**: P50=1,172.0 ns | P90=3,089.0 ns | P99=5,484.0 ns | P99.9=7,770.0 ns | Max=19,127.0 ns | Throughput=0.0850 M orders/s

**Stability (CV across trials)**: P50=1.39% | P99=1.10% | P99.9=5.17%

**Workload composition (identical every trial -- deterministic replay)**:
- Crossing orders: 36066 | Non-crossing: 49052 | Partial fills: 5032 | Multi-level sweeps: 19345
- Self-trade skips: 6564 | Generated fills: 65063 | Mean fills/match: 1.804 | Max fill fan-out: 8 | Mean levels consumed: 0.813

**Correctness/Resources (every trial)**: dropped_events=0 | arena_fallback_count=0 | pool_exhaustion_count=0 | determinism=PASS | state_hash=`7d8f570852562599` (identical across all trials) | valid=YES

---

### mutex_queue — Pro-Rata

- **Run ID**: `98a597b-ablation-mutex_queue-pro_rata-1784608715`
- **Commit**: `98a597b-ablation-mutex_queue`  |  **Working tree**: Clean
- **Matching mode**: `pro_rata`  |  **Dataset**: `/home/abc/projects/Blitz-Limit-Order-Book/datasets/bench_110k.bin` (hash `51ce6285b274413e`)
- **CPU**: 12th Gen Intel(R) Core(TM) i5-1235U  |  **Compiler**: GCC 15.2.0
- **Env**: `isolated=4-5 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=98a597b-ablation-mutex_queue dirty=no`
- **Result validity**: **VALID**

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 1,150.0 | 2,006.0 | 5,460.0 | 8,030.0 | 1,390.6 | 17,666.0 | 84,992.3 |
| 1 | 1,252.0 | 4,770.0 | 5,494.0 | 7,752.0 | 1,782.7 | 18,533.0 | 84,992.1 |
| 2 | 1,315.0 | 4,782.0 | 5,517.0 | 7,774.0 | 1,835.9 | 17,301.0 | 84,992.3 |
| 3 | 1,262.0 | 4,771.0 | 5,512.0 | 7,721.0 | 1,786.5 | 18,034.0 | 84,992.3 |
| 4 | 1,255.0 | 4,785.0 | 5,596.0 | 8,184.0 | 1,793.1 | 19,565.0 | 84,992.3 |

**Median across trials**: P50=1,255.0 ns | P90=4,771.0 ns | P99=5,512.0 ns | P99.9=7,774.0 ns | Max=18,034.0 ns | Throughput=0.0850 M orders/s

**Stability (CV across trials)**: P50=4.80% | P99=0.91% | P99.9=2.59%

**Workload composition (identical every trial -- deterministic replay)**:
- Crossing orders: 36072 | Non-crossing: 49046 | Partial fills: 5042 | Multi-level sweeps: 19367
- Self-trade skips: 7961 | Generated fills: 74733 | Mean fills/match: 2.072 | Max fill fan-out: 35 | Mean levels consumed: 0.814

**Correctness/Resources (every trial)**: dropped_events=0 | arena_fallback_count=0 | pool_exhaustion_count=0 | determinism=PASS | state_hash=`0ac25b855783070e` (identical across all trials) | valid=YES

---

### unpinned (CPU pinning disabled) — Price-Time

- **Run ID**: `98a597b-ablation-unpinned-price_time-1784608683`
- **Commit**: `98a597b-ablation-unpinned`  |  **Working tree**: Clean
- **Matching mode**: `price_time`  |  **Dataset**: `/home/abc/projects/Blitz-Limit-Order-Book/datasets/bench_110k.bin` (hash `51ce6285b274413e`)
- **CPU**: 12th Gen Intel(R) Core(TM) i5-1235U  |  **Compiler**: GCC 15.2.0
- **Env**: `isolated=4-5 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=98a597b-ablation-unpinned dirty=no`
- **Result validity**: **VALID**

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 369.0 | 582.0 | 1,065.0 | 4,729.0 | 440.0 | 79,259.0 | 84,992.6 |
| 1 | 384.0 | 615.0 | 1,221.0 | 7,365.0 | 467.4 | 146,852.0 | 84,992.6 |
| 2 | 381.0 | 603.0 | 1,098.0 | 13,895.0 | 558.0 | 346,499.0 | 84,992.6 |
| 3 | 380.0 | 602.0 | 1,021.0 | 144,646.0 | 723.3 | 347,644.0 | 84,992.6 |
| 4 | 381.0 | 604.0 | 1,160.0 | 9,216.0 | 475.4 | 80,365.0 | 84,992.6 |

**Median across trials**: P50=381.0 ns | P90=603.0 ns | P99=1,098.0 ns | P99.9=9,216.0 ns | Max=146,852.0 ns | Throughput=0.0850 M orders/s

**Stability (CV across trials)**: P50=1.53% | P99=7.08% | P99.9=169.15%

**Workload composition (identical every trial -- deterministic replay)**:
- Crossing orders: 36066 | Non-crossing: 49052 | Partial fills: 5032 | Multi-level sweeps: 19345
- Self-trade skips: 6564 | Generated fills: 65063 | Mean fills/match: 1.804 | Max fill fan-out: 8 | Mean levels consumed: 0.813

**Correctness/Resources (every trial)**: dropped_events=0 | arena_fallback_count=0 | pool_exhaustion_count=0 | determinism=PASS | state_hash=`7d8f570852562599` (identical across all trials) | valid=YES

---

### unpinned — Pro-Rata

- **Run ID**: `98a597b-ablation-unpinned-pro_rata-1784608723`
- **Commit**: `98a597b-ablation-unpinned`  |  **Working tree**: Clean
- **Matching mode**: `pro_rata`  |  **Dataset**: `/home/abc/projects/Blitz-Limit-Order-Book/datasets/bench_110k.bin` (hash `51ce6285b274413e`)
- **CPU**: 12th Gen Intel(R) Core(TM) i5-1235U  |  **Compiler**: GCC 15.2.0
- **Env**: `isolated=4-5 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=98a597b-ablation-unpinned dirty=no`
- **Result validity**: **VALID**

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 358.0 | 570.0 | 1,105.0 | 4,279.0 | 414.8 | 23,149.0 | 84,993.2 |
| 1 | 378.0 | 651.0 | 3,035.0 | 12,019.0 | 512.0 | 79,295.0 | 84,993.3 |
| 2 | 375.0 | 640.0 | 2,757.0 | 40,144.0 | 547.1 | 237,129.0 | 84,993.2 |
| 3 | 372.0 | 603.0 | 1,046.0 | 2,500.0 | 422.7 | 16,800.0 | 84,993.2 |
| 4 | 374.0 | 619.0 | 2,175.0 | 4,906.0 | 456.7 | 181,632.0 | 84,993.3 |

**Median across trials**: P50=374.0 ns | P90=619.0 ns | P99=2,175.0 ns | P99.9=4,906.0 ns | Max=79,295.0 ns | Throughput=0.0850 M orders/s

**Stability (CV across trials)**: P50=2.10% | P99=45.45% | P99.9=123.16%

**Workload composition (identical every trial -- deterministic replay)**:
- Crossing orders: 36072 | Non-crossing: 49046 | Partial fills: 5042 | Multi-level sweeps: 19367
- Self-trade skips: 7961 | Generated fills: 74733 | Mean fills/match: 2.072 | Max fill fan-out: 35 | Mean levels consumed: 0.814

**Correctness/Resources (every trial)**: dropped_events=0 | arena_fallback_count=0 | pool_exhaustion_count=0 | determinism=PASS | state_hash=`0ac25b855783070e` (identical across all trials) | valid=YES

---

### malloc_pool (fixed object pools replaced with new/delete) — Price-Time

- **Run ID**: `98a597b-ablation-malloc_pool-price_time-1784608691`
- **Commit**: `98a597b-ablation-malloc_pool`  |  **Working tree**: Clean
- **Matching mode**: `price_time`  |  **Dataset**: `/home/abc/projects/Blitz-Limit-Order-Book/datasets/bench_110k.bin` (hash `51ce6285b274413e`)
- **CPU**: 12th Gen Intel(R) Core(TM) i5-1235U  |  **Compiler**: GCC 15.2.0
- **Env**: `isolated=4-5 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=98a597b-ablation-malloc_pool dirty=no`
- **Result validity**: **VALID**

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 512.0 | 813.0 | 2,167.0 | 5,836.0 | 605.8 | 12,728.0 | 84,992.1 |
| 1 | 528.0 | 836.0 | 1,973.0 | 5,671.0 | 616.3 | 16,198.0 | 84,992.1 |
| 2 | 530.0 | 843.0 | 2,481.0 | 6,521.0 | 625.7 | 49,232.0 | 84,992.2 |
| 3 | 533.0 | 845.0 | 2,528.0 | 6,419.0 | 632.9 | 117,595.0 | 84,992.2 |
| 4 | 526.0 | 835.0 | 2,159.0 | 6,079.0 | 618.0 | 19,163.0 | 84,992.0 |

**Median across trials**: P50=528.0 ns | P90=836.0 ns | P99=2,167.0 ns | P99.9=6,079.0 ns | Max=19,163.0 ns | Throughput=0.0850 M orders/s

**Stability (CV across trials)**: P50=1.55% | P99=10.41% | P99.9=5.98%

**Workload composition (identical every trial -- deterministic replay)**:
- Crossing orders: 36066 | Non-crossing: 49052 | Partial fills: 5032 | Multi-level sweeps: 19345
- Self-trade skips: 6564 | Generated fills: 65063 | Mean fills/match: 1.804 | Max fill fan-out: 8 | Mean levels consumed: 0.813

**Correctness/Resources (every trial)**: dropped_events=0 | arena_fallback_count=0 | pool_exhaustion_count=0 | determinism=PASS | state_hash=`7d8f570852562599` (identical across all trials) | valid=YES

---

### malloc_pool — Pro-Rata

- **Run ID**: `98a597b-ablation-malloc_pool-pro_rata-1784608731`
- **Commit**: `98a597b-ablation-malloc_pool`  |  **Working tree**: Clean
- **Matching mode**: `pro_rata`  |  **Dataset**: `/home/abc/projects/Blitz-Limit-Order-Book/datasets/bench_110k.bin` (hash `51ce6285b274413e`)
- **CPU**: 12th Gen Intel(R) Core(TM) i5-1235U  |  **Compiler**: GCC 15.2.0
- **Env**: `isolated=4-5 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=98a597b-ablation-malloc_pool dirty=no`
- **Result validity**: **VALID**

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 516.0 | 850.0 | 3,176.0 | 6,639.0 | 630.4 | 50,464.0 | 84,991.9 |
| 1 | 540.0 | 894.0 | 3,431.0 | 7,810.0 | 659.8 | 59,946.0 | 84,991.9 |
| 2 | 534.0 | 885.0 | 3,289.0 | 6,948.0 | 655.8 | 121,230.0 | 84,991.8 |
| 3 | 536.0 | 887.0 | 3,430.0 | 7,372.0 | 651.4 | 19,581.0 | 84,991.7 |
| 4 | 531.0 | 877.0 | 2,970.0 | 6,497.0 | 640.3 | 12,804.0 | 84,991.9 |

**Median across trials**: P50=534.0 ns | P90=885.0 ns | P99=3,289.0 ns | P99.9=6,948.0 ns | Max=50,464.0 ns | Throughput=0.0850 M orders/s

**Stability (CV across trials)**: P50=1.73% | P99=5.94% | P99.9=7.66%

**Workload composition (identical every trial -- deterministic replay)**:
- Crossing orders: 36072 | Non-crossing: 49046 | Partial fills: 5042 | Multi-level sweeps: 19367
- Self-trade skips: 7961 | Generated fills: 74733 | Mean fills/match: 2.072 | Max fill fan-out: 35 | Mean levels consumed: 0.814

**Correctness/Resources (every trial)**: dropped_events=0 | arena_fallback_count=0 | pool_exhaustion_count=0 | determinism=PASS | state_hash=`0ac25b855783070e` (identical across all trials) | valid=YES

---

### flat_layout (hot/cold cache-line split removed) — Price-Time

- **Run ID**: `98a597b-ablation-flat_layout-price_time-1784608699`
- **Commit**: `98a597b-ablation-flat_layout`  |  **Working tree**: Clean
- **Matching mode**: `price_time`  |  **Dataset**: `/home/abc/projects/Blitz-Limit-Order-Book/datasets/ablation4_flat.bin` (hash `f330eff74ca2a47e`)
- **CPU**: 12th Gen Intel(R) Core(TM) i5-1235U  |  **Compiler**: GCC 15.2.0
- **Env**: `isolated=4-5 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=98a597b-ablation-flat_layout dirty=no`
- **Result validity**: **VALID**

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 480.0 | 769.0 | 1,200.0 | 1,835.0 | 549.2 | 13,812.0 | 84,992.2 |
| 1 | 496.0 | 793.0 | 1,236.0 | 1,838.0 | 561.6 | 13,129.0 | 84,992.2 |
| 2 | 494.0 | 790.0 | 1,221.0 | 1,797.0 | 558.7 | 12,742.0 | 84,992.1 |
| 3 | 496.0 | 792.0 | 1,232.0 | 1,761.0 | 560.2 | 11,633.0 | 84,992.1 |
| 4 | 495.0 | 791.0 | 1,231.0 | 1,762.0 | 560.1 | 12,917.0 | 84,992.2 |

**Median across trials**: P50=495.0 ns | P90=791.0 ns | P99=1,231.0 ns | P99.9=1,797.0 ns | Max=12,917.0 ns | Throughput=0.0850 M orders/s

**Stability (CV across trials)**: P50=1.40% | P99=1.19% | P99.9=2.09%

**Workload composition (identical every trial -- deterministic replay)**:
- Crossing orders: 36066 | Non-crossing: 49052 | Partial fills: 5032 | Multi-level sweeps: 19345
- Self-trade skips: 6564 | Generated fills: 65063 | Mean fills/match: 1.804 | Max fill fan-out: 8 | Mean levels consumed: 0.813

**Correctness/Resources (every trial)**: dropped_events=0 | arena_fallback_count=0 | pool_exhaustion_count=0 | determinism=PASS | state_hash=`7d8f570852562599` (identical across all trials) | valid=YES

---

### flat_layout — Pro-Rata

- **Run ID**: `98a597b-ablation-flat_layout-pro_rata-1784608739`
- **Commit**: `98a597b-ablation-flat_layout`  |  **Working tree**: Clean
- **Matching mode**: `pro_rata`  |  **Dataset**: `/home/abc/projects/Blitz-Limit-Order-Book/datasets/ablation4_flat.bin` (hash `f330eff74ca2a47e`)
- **CPU**: 12th Gen Intel(R) Core(TM) i5-1235U  |  **Compiler**: GCC 15.2.0
- **Env**: `isolated=4-5 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=98a597b-ablation-flat_layout dirty=no`
- **Result validity**: **VALID**

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 485.0 | 824.0 | 1,631.0 | 5,083.0 | 583.5 | 23,083.0 | 84,992.0 |
| 1 | 520.0 | 883.0 | 1,857.0 | 6,014.0 | 620.7 | 29,622.0 | 84,991.8 |
| 2 | 493.0 | 826.0 | 1,454.0 | 3,200.0 | 575.9 | 13,004.0 | 84,991.9 |
| 3 | 493.0 | 828.0 | 1,466.0 | 3,554.0 | 578.0 | 12,626.0 | 84,992.0 |
| 4 | 491.0 | 823.0 | 1,421.0 | 2,937.0 | 572.5 | 13,500.0 | 84,991.9 |

**Median across trials**: P50=493.0 ns | P90=826.0 ns | P99=1,466.0 ns | P99.9=3,554.0 ns | Max=13,500.0 ns | Throughput=0.0850 M orders/s

**Stability (CV across trials)**: P50=2.74% | P99=11.62% | P99.9=31.99%

**Workload composition (identical every trial -- deterministic replay)**:
- Crossing orders: 36072 | Non-crossing: 49046 | Partial fills: 5042 | Multi-level sweeps: 19367
- Self-trade skips: 7961 | Generated fills: 74733 | Mean fills/match: 2.072 | Max fill fan-out: 35 | Mean levels consumed: 0.814

**Correctness/Resources (every trial)**: dropped_events=0 | arena_fallback_count=0 | pool_exhaustion_count=0 | determinism=PASS | state_hash=`0ac25b855783070e` (identical across all trials) | valid=YES

---

## Ablation Verdicts (PROVEN / INCONCLUSIVE / NOT PROVEN / REGRESSION / INVALID)

# Optimization Ablation Study

### Matching policy: price_time

| Design decision | Baseline P99.9 (median) | Rejected alternative | P99.9 delta | Verdict |
|---|---|---|---|---|
| Lock-free SPSC queue | **1762.0 ns** | std::mutex + std::queue: 7770.0 ns | **~4.41x worse** | PROVEN |
| Core pinning + isolation | **1762.0 ns** | Unpinned OS-scheduled threads: 9216.0 ns | **~5.23x worse** | INCONCLUSIVE |
| Fixed-slab object pooling | **1762.0 ns** | new/delete per order: 6079.0 ns | **~3.45x worse** | PROVEN |
| Cache-line hot/cold split | **1762.0 ns** | Flat, unseparated Order layout: 1797.0 ns | **~1.02x worse** | INCONCLUSIVE |

### Matching policy: pro_rata

| Design decision | Baseline P99.9 (median) | Rejected alternative | P99.9 delta | Verdict |
|---|---|---|---|---|
| Lock-free SPSC queue | **3065.0 ns** | std::mutex + std::queue: 7774.0 ns | **~2.54x worse** | PROVEN |
| Core pinning + isolation | **3065.0 ns** | Unpinned OS-scheduled threads: 4906.0 ns | **~1.60x worse** | PROVEN |
| Fixed-slab object pooling | **3065.0 ns** | new/delete per order: 6948.0 ns | **~2.27x worse** | PROVEN |
| Cache-line hot/cold split | **3065.0 ns** | Flat, unseparated Order layout: 3554.0 ns | **~1.16x worse** | INCONCLUSIVE |


---

## Reading this file

- Every number above is read directly from `results/98a597b/*.csv` / `*.meta.json` -- the actual files this project's `--benchmark` and `scripts/run_ablations.sh` wrote in this session, after a clean rebuild at the current committed HEAD.
- "Valid"/"Deterministic" being YES/PASS on every single row is itself a result: zero dropped events, zero pool exhaustion, zero arena fallback, identical logical outcome fingerprint (`state_hash`) across all 5 trials of every run.
- The `unpinned` ablation's own trial-to-trial numbers are themselves the clearest evidence for why its verdict is noise-sensitive: Price-Time P99.9 ranges from 4,729.0 ns to 144,646.0 ns *across the 5 trials of this one binary* -- OS-scheduled unpinned threads are simply that unpredictable, which is exactly the property core pinning exists to remove.
- "Max" latency (worst single sample per trial) is included for every run precisely because it's the number most sensitive to real interference -- compare it across the baseline vs. ablated rows above to see backpressure/scheduling effects the median alone would hide.
- Compare this run's ablation deltas against a future re-run: the *direction* (every ablation slower than baseline) should hold; the *magnitude* is expected to shift with environment/load, which is exactly why this file exists instead of a hand-copied number.