# Blitz LOB: Consolidated Benchmark Results

Per-trial results for every benchmark run in `results_new/77eb286/` (the run of record), a comparison with the repeat runs of the same code, and a comparison with an earlier engine. Every number here is computed from the saved `*.csv`, `*.raw.csv` and `*.meta.json` files; nothing was re-run to write this file, and each per-trial percentile was also recomputed from the raw samples and matches the CSV. Field and statistic definitions are in [METHODOLOGY.md](METHODOLOGY.md).

## Run of record

| Item | Value |
|---|---|
| Commit | `77eb286`, working tree clean (`dirty: false` in every `meta.json`); ablation variants carry the commit string `77eb286-ablation-<variant>` |
| Run date | 2026-11-12 (UTC; run ids carry the Unix start time 1794497477 = 15:31 UTC for the price-time canonical run) |
| Host | 12th Gen Intel Core i5-1235U (hybrid), kernel 7.0.0-43-generic, GCC 15.2.0, Release build |
| Boot parameters | `isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7` (cores 4-7 are one E-core cluster sharing a 2 MB L2; the preflight snapshot records `isolated=4-7`) |
| Pinned cores | producer on core 4, matching thread on core 5, `performance` governor on both (recorded in the preflight snapshot), no SMT sibling on either, one NUMA node (`numactl --cpunodebind=0 --membind=0`, operator-bound, not self-verified) |
| Host tuning | the whole isolated cluster was tuned with `sudo scripts/tune_host.sh 4,5,6,7`: `performance` governor and energy-performance preference on cores 4-7, NMI watchdog off, power-profiles-daemon masked, irqbalance stopped, movable IRQs moved to the other CPUs. Only NVMe queue IRQs, which fire on disk I/O only, may remain on cores 4-7. The tuning itself is an operator step; the run records only the governors of the two pinned cores |
| Clock | `rdtscp_calibrated`, about 30.4 ns of overhead per timestamp |
| Dataset | `datasets/bench_110k.bin`, seed 42, hash `51ce6285b274413e`: 110,000 events = 10,000 warmup + 100,000 measured per trial (85,118 new orders, 14,882 cancels, 0 replaces) |
| Flat-layout dataset | `ablation4_flat.bin` (88-byte records), hash `f330eff74ca2a47e`, written by `scripts/run_ablations.sh` next to the results |
| Runs | 12: 2 canonical (`price_time`, `pro_rata`) + 10 ablation (baseline + 4 variants, both modes); 5 trials each = 60 measured trials, 6,000,000 measured events |
| Latency samples | new orders only: 85,118 per trial. Cancels are in the raw files but excluded from every headline figure |
| Offered load | paced replay at about 85,000 orders/s (the `Throughput` column is that offered rate, not a capacity limit; saturation was not measured) |

All 12 runs are `VALID` with determinism `PASS`, zero dropped events, zero arena fallback and zero pool exhaustion (checked over all 60 trials). `reference_validation_status` is `NOT_RUN` in every row: there is no independent reference matcher in the repository, so `state_hash` is a determinism fingerprint, not a correctness oracle.

## Reading this file

- Units are nanoseconds, measured end to end from the producer's timestamp `t1` (taken when the producer leaves its spin) to the matching thread's timestamp after `match()` returns. The window includes the harness's own serialized timestamps (about 30 ns each); see [METHODOLOGY.md](METHODOLOGY.md).
- Percentiles use the index rule `sorted[floor(p/100 * n)]` over one trial's 85,118 new-order samples. `Median` rows are the median of the five per-trial values, which is how the headline figures are formed. P99.9 rests on about 85 samples per trial and P99.99 on about 9, so P99.99 is indicative only.
- `CV` is the sample standard deviation over the mean of the five per-trial values. The stability verdict is the benchmark's own rule: STABLE if the CV of per-trial P99 and P99.9 are both below 15% (descriptive, not a statistical test).
- The canonical run and the ablation baseline are separate runs of the same binary at different times, so they differ slightly (price-time P99.9 1,158 vs 1,148, pro-rata 2,433 vs 2,343). Ablation ratios are always computed against the ablation baseline, never the canonical run.
- Throughput is the paced input rate; `Max` is the single worst sample per trial and is dominated by host noise.
- `VALID`/`PASS` on every row is a result in itself. `state_hash` identical across trials shows deterministic replay of the same events, not that the matching is right.
- Limits: one host, one day, five trials per run, synthetic data, in-process latency (no network or market-data path). The same code was measured three times on the same day on a tuned host (see [the repeat-runs section](#repeat-runs-of-the-same-code)); there is no repeat on another day.

## At a glance

Median over 5 trials of the per-trial percentile (ns). Source: `results_new/77eb286/`.

| # | Run | P50 | P90 | P99 | P99.9 | Stability | Valid | Determinism |
|---|---|---|---|---|---|---|---|---|
| 1 | **Canonical Price-Time** | 320 | 549 | 851 | 1,158 | STABLE | YES | PASS |
| 2 | **Canonical Pro-Rata** | 325 | 591 | 1,078 | 2,433 | STABLE | YES | PASS |
| 3 | Ablation baseline Price-Time | 320 | 549 | 851 | 1,148 | STABLE | YES | PASS |
| 4 | Ablation baseline Pro-Rata | 326 | 594 | 1,075 | 2,343 | STABLE | YES | PASS |
| 5 | Ablation mutex_queue Price-Time | 861 | 1,466 | 1,867 | 2,246 | STABLE | YES | PASS |
| 6 | Ablation mutex_queue Pro-Rata | 844 | 1,487 | 2,115 | 3,363 | STABLE | YES | PASS |
| 7 | Ablation unpinned Price-Time | 248 | 413 | 675 | 2,435 | UNSTABLE | YES | PASS |
| 8 | Ablation unpinned Pro-Rata | 251 | 443 | 806 | 1,782 | STABLE | YES | PASS |
| 9 | Ablation malloc_pool Price-Time | 348 | 593 | 1,366 | 4,165 | STABLE | YES | PASS |
| 10 | Ablation malloc_pool Pro-Rata | 351 | 634 | 2,247 | 4,330 | STABLE | YES | PASS |
| 11 | Ablation flat_layout Price-Time | 320 | 545 | 838 | 1,120 | STABLE | YES | PASS |
| 12 | Ablation flat_layout Pro-Rata | 325 | 592 | 1,068 | 2,268 | STABLE | YES | PASS |

- Canonical Price-Time P99.99 (indicative, recomputed from `price_time.raw.csv`): 1,611 ns.
- Canonical Pro-Rata P99.99 (indicative, recomputed from `pro_rata.raw.csv`): 5,162 ns.

One run is UNSTABLE: ablation unpinned price-time (per-trial P99.9 CV 130%, see the verdict section). Every other run, including both canonical runs, is STABLE.

---

## Canonical runs (the headline per matching policy)

### 1. Canonical Price-Time (FIFO)

- **Run ID**: `77eb286-price_time-1794497477`  |  **Commit string**: `77eb286`  |  **Working tree**: clean
- **Mode**: `price_time`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=77eb286 dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 320 | 546 | 843 | 1,149 | 368.5 | 4,753 | 84,991.7 |
| 1 | 318 | 549 | 858 | 1,216 | 369.9 | 12,059 | 84,992.0 |
| 2 | 319 | 548 | 851 | 1,155 | 369.1 | 8,342 | 84,991.7 |
| 3 | 320 | 551 | 853 | 1,179 | 371.0 | 6,047 | 84,991.8 |
| 4 | 320 | 549 | 850 | 1,158 | 370.5 | 3,289 | 84,992.0 |
| **Median** | **320** | **549** | **851** | **1,158** | 369.9 | 6,047 | 84,991.8 |
| CV across trials | 0.28% | 0.33% | 0.64% | 2.34% | 0.27% | 49.73% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 0.64%, P99.9 = 2.34%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,743 | non-crossing 49,375 | partial fills 4,895 | multi-level sweeps 19,117 | FOK orders 1,800 | self-trade skips 6,507  
Generated fills 64,420 | mean fills per crossing order 1.802 | max fill fan-out 8 | mean levels consumed 0.805

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `8b74c1ea41c568fe` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 2. Canonical Pro-Rata (proportional allocation)

- **Run ID**: `77eb286-pro_rata-1794497485`  |  **Commit string**: `77eb286`  |  **Working tree**: clean
- **Mode**: `pro_rata`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=77eb286 dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 325 | 589 | 1,082 | 2,543 | 390.8 | 13,186 | 84,991.9 |
| 1 | 324 | 590 | 1,074 | 2,377 | 390.0 | 9,345 | 84,991.9 |
| 2 | 325 | 591 | 1,072 | 2,433 | 390.6 | 12,067 | 84,991.9 |
| 3 | 326 | 592 | 1,078 | 2,500 | 392.0 | 11,228 | 84,991.9 |
| 4 | 326 | 592 | 1,080 | 2,403 | 391.7 | 11,711 | 84,992.1 |
| **Median** | **325** | **591** | **1,078** | **2,433** | 390.8 | 11,711 | 84,991.9 |
| CV across trials | 0.26% | 0.22% | 0.39% | 2.81% | 0.21% | 12.23% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 0.39%, P99.9 = 2.81%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,750 | non-crossing 49,368 | partial fills 4,901 | multi-level sweeps 19,154 | FOK orders 1,800 | self-trade skips 8,032  
Generated fills 74,817 | mean fills per crossing order 2.093 | max fill fan-out 44 | mean levels consumed 0.806

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `6396454671e3e128` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

- Price-Time order types from `price_time.console.txt`: GTC 79,185, IOC 4,133, FOK 1,800 (sum 85,118 = new orders). The console's stability verdict (STABLE) and CVs agree with the values above.
- Pro-Rata order types from `pro_rata.console.txt`: GTC 79,185, IOC 4,133, FOK 1,800 (sum 85,118 = new orders). The console's stability verdict (STABLE) and CVs agree with the values above.

Price-time per-trial P99.9 runs from 1,149 to 1,216 ns (trial 1 is the high one), pro-rata from 2,377 to 2,543 ns; both are tight enough that the medians are not driven by a single trial.

The two canonical policies replay the same 110,000 events; they differ only in how a crossing order is allocated. Pro-rata gives a floor share to nearly every eligible resting order, so one crossing order produces many more fills (max fan-out 44 against 8), which is why its P99 and P99.9 are higher. Each fill also costs about 37 ns in the harness's fill callback (two serialized timestamps and a digest fold), which is inside the timed window.

---

## Ablation suite (baseline + 4 variants, both policies)

Each variant is a separate build of the engine with one design decision reversed. All were run in one pass of `scripts/run_ablations.sh --trials 5` (`RESULTS_ROOT=results_new`). Every arm transports the same eight fields through its queue, so each ablation changes one thing: the production SPSC ring is one 64-byte slot per event and serves both `Order` layouts (production 128 bytes and flat 88 bytes), and the mutex arm queues the same eight-field payload through `std::mutex` + `std::queue` (a `std::deque` of that payload).

### 3. Ablation baseline: all optimizations on, Price-Time

- **Run ID**: `77eb286-price_time-1794497507`  |  **Commit string**: `77eb286`  |  **Working tree**: clean
- **Mode**: `price_time`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=77eb286 dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 320 | 546 | 844 | 1,157 | 369.1 | 11,588 | 84,991.8 |
| 1 | 319 | 547 | 848 | 1,129 | 368.9 | 6,928 | 84,991.9 |
| 2 | 320 | 549 | 851 | 1,143 | 369.9 | 5,309 | 84,991.8 |
| 3 | 320 | 550 | 854 | 1,148 | 370.6 | 1,818 | 84,992.0 |
| 4 | 321 | 551 | 852 | 1,155 | 371.6 | 7,068 | 84,991.8 |
| **Median** | **320** | **549** | **851** | **1,148** | 369.9 | 6,928 | 84,991.8 |
| CV across trials | 0.22% | 0.38% | 0.46% | 0.98% | 0.30% | 53.89% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 0.46%, P99.9 = 0.98%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,743 | non-crossing 49,375 | partial fills 4,895 | multi-level sweeps 19,117 | FOK orders 1,800 | self-trade skips 6,507  
Generated fills 64,420 | mean fills per crossing order 1.802 | max fill fan-out 8 | mean levels consumed 0.805

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `8b74c1ea41c568fe` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 4. Ablation baseline: all optimizations on, Pro-Rata

- **Run ID**: `77eb286-pro_rata-1794497547`  |  **Commit string**: `77eb286`  |  **Working tree**: clean
- **Mode**: `pro_rata`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=77eb286 dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 326 | 591 | 1,066 | 2,329 | 390.3 | 9,320 | 84,991.6 |
| 1 | 324 | 594 | 1,075 | 2,305 | 390.2 | 10,460 | 84,991.7 |
| 2 | 326 | 594 | 1,078 | 2,386 | 391.6 | 10,407 | 84,991.9 |
| 3 | 326 | 596 | 1,071 | 2,343 | 392.3 | 8,534 | 84,992.0 |
| 4 | 327 | 596 | 1,080 | 2,440 | 393.2 | 10,121 | 84,991.8 |
| **Median** | **326** | **594** | **1,075** | **2,343** | 391.6 | 10,121 | 84,991.8 |
| CV across trials | 0.34% | 0.34% | 0.52% | 2.26% | 0.33% | 8.47% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 0.52%, P99.9 = 2.26%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,750 | non-crossing 49,368 | partial fills 4,901 | multi-level sweeps 19,154 | FOK orders 1,800 | self-trade skips 8,032  
Generated fills 74,817 | mean fills per crossing order 2.093 | max fill fan-out 44 | mean levels consumed 0.806

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `6396454671e3e128` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 5. Ablation mutex_queue: lock-free SPSC ring replaced by std::mutex + std::queue (same eight-field payload), Price-Time

- **Run ID**: `77eb286-ablation-mutex_queue-price_time-1794497515`  |  **Commit string**: `77eb286-ablation-mutex_queue`  |  **Working tree**: clean
- **Mode**: `price_time`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=77eb286-ablation-mutex_queue dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 867 | 1,445 | 1,845 | 2,172 | 934.3 | 12,750 | 84,991.9 |
| 1 | 854 | 1,484 | 1,888 | 2,287 | 942.2 | 16,575 | 84,992.1 |
| 2 | 861 | 1,466 | 1,867 | 2,231 | 937.1 | 11,552 | 84,992.3 |
| 3 | 853 | 1,457 | 1,864 | 2,246 | 933.0 | 17,856 | 84,992.2 |
| 4 | 861 | 1,468 | 1,869 | 2,246 | 938.9 | 17,793 | 84,992.0 |
| **Median** | **861** | **1,466** | **1,867** | **2,246** | 937.1 | 16,575 | 84,992.1 |
| CV across trials | 0.67% | 0.98% | 0.82% | 1.86% | 0.39% | 19.31% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 0.82%, P99.9 = 1.86%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,743 | non-crossing 49,375 | partial fills 4,895 | multi-level sweeps 19,117 | FOK orders 1,800 | self-trade skips 6,507  
Generated fills 64,420 | mean fills per crossing order 1.802 | max fill fan-out 8 | mean levels consumed 0.805

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `8b74c1ea41c568fe` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 6. Ablation mutex_queue: lock-free SPSC ring replaced by std::mutex + std::queue (same eight-field payload), Pro-Rata

- **Run ID**: `77eb286-ablation-mutex_queue-pro_rata-1794497555`  |  **Commit string**: `77eb286-ablation-mutex_queue`  |  **Working tree**: clean
- **Mode**: `pro_rata`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=77eb286-ablation-mutex_queue dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 853 | 1,492 | 2,130 | 3,307 | 952.5 | 13,424 | 84,992.0 |
| 1 | 843 | 1,487 | 2,106 | 3,343 | 946.5 | 12,244 | 84,992.0 |
| 2 | 844 | 1,486 | 2,115 | 3,393 | 946.6 | 28,615 | 84,992.1 |
| 3 | 842 | 1,480 | 2,079 | 3,389 | 944.3 | 12,663 | 84,992.1 |
| 4 | 848 | 1,487 | 2,127 | 3,363 | 948.9 | 13,417 | 84,992.0 |
| **Median** | **844** | **1,487** | **2,115** | **3,363** | 946.6 | 13,417 | 84,992.0 |
| CV across trials | 0.54% | 0.29% | 0.97% | 1.06% | 0.33% | 43.74% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 0.97%, P99.9 = 1.06%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,750 | non-crossing 49,368 | partial fills 4,901 | multi-level sweeps 19,154 | FOK orders 1,800 | self-trade skips 8,032  
Generated fills 74,817 | mean fills per crossing order 2.093 | max fill fan-out 44 | mean levels consumed 0.806

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `6396454671e3e128` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 7. Ablation unpinned: thread pinning removed (OS-scheduled threads), Price-Time

- **Run ID**: `77eb286-ablation-unpinned-price_time-1794497523`  |  **Commit string**: `77eb286-ablation-unpinned`  |  **Working tree**: clean
- **Mode**: `price_time`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=77eb286-ablation-unpinned dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 250 | 411 | 675 | 52,559 | 406.7 | 181,756 | 84,992.7 |
| 1 | 248 | 416 | 694 | 69,960 | 455.4 | 271,871 | 84,992.6 |
| 2 | 252 | 425 | 730 | 2,435 | 303.5 | 68,655 | 84,992.7 |
| 3 | 248 | 413 | 667 | 1,682 | 292.9 | 23,065 | 84,992.7 |
| 4 | 248 | 413 | 662 | 1,161 | 290.9 | 18,074 | 84,992.7 |
| **Median** | **248** | **413** | **675** | **2,435** | 303.5 | 68,655 | 84,992.7 |
| CV across trials | 0.72% | 1.34% | 4.03% | 129.77% | 21.79% | 98.25% | 0.000% |

**Stability verdict**: UNSTABLE (CV of per-trial P99 = 4.03%, P99.9 = 129.77%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,743 | non-crossing 49,375 | partial fills 4,895 | multi-level sweeps 19,117 | FOK orders 1,800 | self-trade skips 6,507  
Generated fills 64,420 | mean fills per crossing order 1.802 | max fill fan-out 8 | mean levels consumed 0.805

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `8b74c1ea41c568fe` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 8. Ablation unpinned: thread pinning removed (OS-scheduled threads), Pro-Rata

- **Run ID**: `77eb286-ablation-unpinned-pro_rata-1794497563`  |  **Commit string**: `77eb286-ablation-unpinned`  |  **Working tree**: clean
- **Mode**: `pro_rata`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=77eb286-ablation-unpinned dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 253 | 444 | 821 | 1,837 | 303.5 | 27,259 | 84,992.7 |
| 1 | 251 | 443 | 806 | 1,782 | 300.9 | 28,218 | 84,992.7 |
| 2 | 251 | 445 | 812 | 1,861 | 302.5 | 25,602 | 84,992.7 |
| 3 | 251 | 443 | 800 | 1,781 | 301.0 | 33,950 | 84,992.7 |
| 4 | 252 | 443 | 799 | 1,720 | 300.5 | 12,061 | 84,992.7 |
| **Median** | **251** | **443** | **806** | **1,782** | 301.0 | 27,259 | 84,992.7 |
| CV across trials | 0.36% | 0.20% | 1.13% | 3.06% | 0.43% | 31.87% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 1.13%, P99.9 = 3.06%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,750 | non-crossing 49,368 | partial fills 4,901 | multi-level sweeps 19,154 | FOK orders 1,800 | self-trade skips 8,032  
Generated fills 74,817 | mean fills per crossing order 2.093 | max fill fan-out 44 | mean levels consumed 0.806

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `6396454671e3e128` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 9. Ablation malloc_pool: fixed object pools replaced by new/delete, Price-Time

- **Run ID**: `77eb286-ablation-malloc_pool-price_time-1794497531`  |  **Commit string**: `77eb286-ablation-malloc_pool`  |  **Working tree**: clean
- **Mode**: `price_time`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=77eb286-ablation-malloc_pool dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 348 | 591 | 1,371 | 4,237 | 429.7 | 61,526 | 84,991.7 |
| 1 | 347 | 592 | 1,366 | 4,094 | 434.7 | 111,980 | 84,991.9 |
| 2 | 347 | 593 | 1,366 | 4,165 | 467.9 | 252,569 | 84,991.9 |
| 3 | 348 | 595 | 1,315 | 4,203 | 428.8 | 17,903 | 84,991.9 |
| 4 | 348 | 595 | 1,337 | 4,043 | 428.1 | 6,726 | 84,991.8 |
| **Median** | **348** | **593** | **1,366** | **4,165** | 429.7 | 61,526 | 84,991.9 |
| CV across trials | 0.16% | 0.30% | 1.79% | 1.91% | 3.89% | 110.74% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 1.79%, P99.9 = 1.91%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,743 | non-crossing 49,375 | partial fills 4,895 | multi-level sweeps 19,117 | FOK orders 1,800 | self-trade skips 6,507  
Generated fills 64,420 | mean fills per crossing order 1.802 | max fill fan-out 8 | mean levels consumed 0.805

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `8b74c1ea41c568fe` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 10. Ablation malloc_pool: fixed object pools replaced by new/delete, Pro-Rata

- **Run ID**: `77eb286-ablation-malloc_pool-pro_rata-1794497571`  |  **Commit string**: `77eb286-ablation-malloc_pool`  |  **Working tree**: clean
- **Mode**: `pro_rata`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=77eb286-ablation-malloc_pool dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 351 | 630 | 2,247 | 4,360 | 447.3 | 19,818 | 84,991.8 |
| 1 | 350 | 634 | 2,371 | 4,330 | 448.8 | 58,398 | 84,991.8 |
| 2 | 351 | 634 | 2,164 | 4,095 | 449.8 | 75,057 | 84,991.9 |
| 3 | 351 | 636 | 2,353 | 4,425 | 469.9 | 178,913 | 84,992.0 |
| 4 | 352 | 635 | 2,165 | 4,068 | 447.6 | 11,026 | 84,991.7 |
| **Median** | **351** | **634** | **2,247** | **4,330** | 448.8 | 58,398 | 84,991.8 |
| CV across trials | 0.20% | 0.36% | 4.39% | 3.83% | 2.14% | 97.75% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 4.39%, P99.9 = 3.83%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,750 | non-crossing 49,368 | partial fills 4,901 | multi-level sweeps 19,154 | FOK orders 1,800 | self-trade skips 8,032  
Generated fills 74,817 | mean fills per crossing order 2.093 | max fill fan-out 44 | mean levels consumed 0.806

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `6396454671e3e128` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 11. Ablation flat_layout: hot/cold cache-line split removed (flat 88-byte Order, same 64-byte-slot queue), Price-Time

- **Run ID**: `77eb286-ablation-flat_layout-price_time-1794497539`  |  **Commit string**: `77eb286-ablation-flat_layout`  |  **Working tree**: clean
- **Mode**: `price_time`  |  **Dataset**: `results_new/77eb286/ablation4_flat.bin` (hash `f330eff74ca2a47e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=77eb286-ablation-flat_layout dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 320 | 544 | 845 | 1,213 | 368.8 | 10,179 | 84,991.9 |
| 1 | 319 | 543 | 834 | 1,120 | 367.1 | 12,377 | 84,992.0 |
| 2 | 319 | 546 | 838 | 1,130 | 368.3 | 1,894 | 84,992.1 |
| 3 | 320 | 546 | 839 | 1,111 | 368.9 | 9,535 | 84,992.0 |
| 4 | 320 | 545 | 836 | 1,110 | 368.9 | 1,552 | 84,992.1 |
| **Median** | **320** | **545** | **838** | **1,120** | 368.8 | 9,535 | 84,992.0 |
| CV across trials | 0.17% | 0.24% | 0.50% | 3.81% | 0.21% | 70.75% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 0.50%, P99.9 = 3.81%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,743 | non-crossing 49,375 | partial fills 4,895 | multi-level sweeps 19,117 | FOK orders 1,800 | self-trade skips 6,507  
Generated fills 64,420 | mean fills per crossing order 1.802 | max fill fan-out 8 | mean levels consumed 0.805

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `8b74c1ea41c568fe` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 12. Ablation flat_layout: hot/cold cache-line split removed (flat 88-byte Order, same 64-byte-slot queue), Pro-Rata

- **Run ID**: `77eb286-ablation-flat_layout-pro_rata-1794497579`  |  **Commit string**: `77eb286-ablation-flat_layout`  |  **Working tree**: clean
- **Mode**: `pro_rata`  |  **Dataset**: `results_new/77eb286/ablation4_flat.bin` (hash `f330eff74ca2a47e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=77eb286-ablation-flat_layout dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 325 | 589 | 1,062 | 2,302 | 388.9 | 10,980 | 84,992.1 |
| 1 | 324 | 591 | 1,065 | 2,263 | 388.7 | 9,301 | 84,992.0 |
| 2 | 325 | 592 | 1,068 | 2,304 | 389.7 | 9,073 | 84,992.0 |
| 3 | 325 | 597 | 1,077 | 2,261 | 391.3 | 6,854 | 84,992.1 |
| 4 | 326 | 593 | 1,070 | 2,268 | 391.0 | 7,482 | 84,992.2 |
| **Median** | **325** | **592** | **1,068** | **2,268** | 389.7 | 9,073 | 84,992.1 |
| CV across trials | 0.22% | 0.50% | 0.53% | 0.94% | 0.31% | 18.61% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 0.53%, P99.9 = 0.94%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,750 | non-crossing 49,368 | partial fills 4,901 | multi-level sweeps 19,154 | FOK orders 1,800 | self-trade skips 8,032  
Generated fills 74,817 | mean fills per crossing order 2.093 | max fill fan-out 44 | mean levels consumed 0.806

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `6396454671e3e128` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

---

## Ablation verdicts

The rule (`scripts/ablation_verdict.py`, mirroring `hydra::stats::classify_ablation` in `include/hydra/stats.hpp`; both are checked against the shared 53-case fixture `tests/verdict_cases.json`) compares the per-trial P99.9 of the ablated build with the baseline, 5 trials per arm, first match wins:

| Verdict | Condition |
|---|---|
| INVALID | an arm failed the validity gate (valid, determinism, no dropped events, no arena fallback, no pool exhaustion), has no trials, or the baseline median is 0 or less |
| INCONCLUSIVE | fewer than 5 trials in an arm |
| NO_MEASURABLE_EFFECT | median ablated P99.9 within +-10% of the baseline median (integer-percent test) |
| PROVEN | median ablated >= 110% of baseline **and** every ablated trial above every baseline trial (with 5 vs 5 trials, complete separation has chance probability 1/252) |
| ABLATED_FASTER | the mirror: median <= baseline / 1.10 and every ablated trial below every baseline trial |
| INCONCLUSIVE | a material median gap whose per-trial ranges overlap |

Limits: 5 trials per arm; the 10% band is a convention; NO_MEASURABLE_EFFECT means not detected, not equal; one huge outlier trial can hide behind an in-band median; one host, one session. The earlier rule gated on P99 and a CV noise floor, hid a 40x tail penalty, and flipped on noise, so it was replaced.

### Verdict inputs (run of record, `results_new/77eb286/`)

Per-trial P99.9 in ns, trial order 0 to 4. Ratio = median ablated / median baseline. The verdicts below were recomputed here from the per-trial CSV values and agree with `ablation_summary.csv`, `ablation_summary.md` and `ablation_raw.jsonl` for all 8 cells.

#### Price-Time

Baseline per-trial P99.9: [1,157, 1,129, 1,143, 1,148, 1,155], median **1,148**, range 1,129 to 1,157.

| Design decision | Ablated per-trial P99.9 | Median | Range | Ratio | Median >= 110% (or <= 1/1.10)? | Trials fully separated? | Separation margin | Verdict |
|---|---|---|---|---|---|---|---|---|
| Lock-free SPSC queue (`mutex_queue`) | [2,172, 2,287, 2,231, 2,246, 2,246] | 2,246 | 2,172 to 2,287 | 1.96x | yes (slower) | yes | 1,015 ns (fastest ablated 2,172 vs slowest baseline 1,157) | **PROVEN** |
| Core pinning + isolation (`unpinned`) | [52,559, 69,960, 2,435, 1,682, 1,161] | 2,435 | 1,161 to 69,960 | 2.12x | yes (slower) | yes | 4 ns (fastest ablated 1,161 vs slowest baseline 1,157) | **PROVEN** |
| Fixed-slab object pooling (`malloc_pool`) | [4,237, 4,094, 4,165, 4,203, 4,043] | 4,165 | 4,043 to 4,237 | 3.63x | yes (slower) | yes | 2,886 ns (fastest ablated 4,043 vs slowest baseline 1,157) | **PROVEN** |
| Cache-line hot/cold split (`flat_layout`) | [1,213, 1,120, 1,130, 1,111, 1,110] | 1,120 | 1,110 to 1,213 | 0.98x | no (within +-10%) | not needed | n/a | **NO_MEASURABLE_EFFECT** |

#### Pro-Rata

Baseline per-trial P99.9: [2,329, 2,305, 2,386, 2,343, 2,440], median **2,343**, range 2,305 to 2,440.

| Design decision | Ablated per-trial P99.9 | Median | Range | Ratio | Median >= 110% (or <= 1/1.10)? | Trials fully separated? | Separation margin | Verdict |
|---|---|---|---|---|---|---|---|---|
| Lock-free SPSC queue (`mutex_queue`) | [3,307, 3,343, 3,393, 3,389, 3,363] | 3,363 | 3,307 to 3,393 | 1.44x | yes (slower) | yes | 867 ns (fastest ablated 3,307 vs slowest baseline 2,440) | **PROVEN** |
| Core pinning + isolation (`unpinned`) | [1,837, 1,782, 1,861, 1,781, 1,720] | 1,782 | 1,720 to 1,861 | 0.76x | yes (faster) | yes | 444 ns (slowest ablated 1,861 vs fastest baseline 2,305) | **ABLATED_FASTER** |
| Fixed-slab object pooling (`malloc_pool`) | [4,360, 4,330, 4,095, 4,425, 4,068] | 4,330 | 4,068 to 4,425 | 1.85x | yes (slower) | yes | 1,628 ns (fastest ablated 4,068 vs slowest baseline 2,440) | **PROVEN** |
| Cache-line hot/cold split (`flat_layout`) | [2,302, 2,263, 2,304, 2,261, 2,268] | 2,268 | 2,261 to 2,304 | 0.97x | no (within +-10%) | not needed | n/a | **NO_MEASURABLE_EFFECT** |

Reading the table: the mutex queue and the heap-allocated pools are PROVEN tail penalties in both policies. The mutex queue is also the clearest effect at the median: P50 861 ns against 320 ns in price-time (2.7x) and 844 against 326 in pro-rata (2.6x). Both queue arms carry the same eight-field payload, so the difference is the synchronization, not the copy size.

The hot/cold layout split shows no measurable effect (0.98x and 0.97x at P99.9, inside the band). The comparison is clean: the flat arm uses the same queue as the baseline, and its P50 is 320 ns against 320 for the baseline in price-time (325 against 326 in pro-rata), so there is no effect at any percentile. That is a not-detected result on this workload, and the reason is a footprint argument rather than a line-count one (see [DESIGN.md](DESIGN.md)).

The `unpinned` row needs care and must not be read as a measured effect of pinning. In price-time it is PROVEN worse (2.12x), but only by a margin of 4 ns: the fastest unpinned trial (1,161 ns) is 4 ns above the slowest baseline trial (1,157 ns), and the unpinned trials range from 1,161 to 69,960 ns (P99.9 CV 130%, the one UNSTABLE run in this file). In pro-rata the unpinned build was faster (0.76x, ABLATED_FASTER). Its P50 is the lowest of all runs (248 and 251 ns against 320 and 326) because the OS placed the threads on faster P-cores, not because unpinning is an advantage. The same row read INCONCLUSIVE in price-time in the two earlier runs of this code and ABLATED_FASTER or INCONCLUSIVE in pro-rata (see [the repeat-runs section](#repeat-runs-of-the-same-code)): it flips between runs because it measures isolation of cores 4-7 plus placement luck on a busy desktop, not pinning.

`meta.json` for the unpinned variant still prints configured cores 4 and 5: the preflight checks the configured cores, not the actual affinity mask.

---

## Comparison with the earlier run (98a597b)

> **This is not a controlled A/B.** `results/98a597b/` is an earlier engine (before the correctness fixes and the performance work listed below) measured in an earlier session, on the same machine and dataset (hash `51ce6285b274413e`). The engine, the build, the day and the host tuning all differ: that run isolated cores 4-5 only (`isolated=4-5` in its metadata), while the final run isolated 4-7 and tuned the whole cluster. Treat the numbers as informative; they do not isolate any single change.

Both sets: median over 5 trials of the per-trial percentile (ns), new orders only. P99.99 is recomputed from the raw samples with the snippet in [METHODOLOGY.md](METHODOLOGY.md#recomputing-a-percentile-from-raw-samples) and is indicative only (about 9 samples per trial). Sources: `results/98a597b/{price_time,pro_rata}.csv` and `results_new/77eb286/{price_time,pro_rata}.csv`.

| Policy | Run | P50 | P90 | P99 | P99.9 | P99.99 (indicative) |
|---|---|---|---|---|---|---|
| Price-Time | earlier, `98a597b` | 485 | 786 | 1,225 | 1,850 | 8,282 |
| Price-Time | run of record, `77eb286` | 320 | 549 | 851 | 1,158 | 1,611 |
| | **new / earlier** | 0.66x | 0.70x | 0.69x | 0.63x | 0.19x |
| | **change** | -34% | -30% | -31% | -37% | -81% |
| Pro-Rata | earlier, `98a597b` | 491 | 826 | 1,425 | 3,132 | 9,708 |
| Pro-Rata | run of record, `77eb286` | 325 | 591 | 1,078 | 2,433 | 5,162 |
| | **new / earlier** | 0.66x | 0.72x | 0.76x | 0.78x | 0.53x |
| | **change** | -34% | -28% | -24% | -22% | -47% |

What also changed between the two canonical runs (same dataset, so these differences come from the engine):

| Item | Earlier `98a597b` | `77eb286` |
|---|---|---|
| Crossing orders (price-time / pro-rata) | 36,066 / 36,072 | 35,743 / 35,750 |
| Generated fills (price-time / pro-rata) | 65,063 / 74,733 | 64,420 / 74,817 |
| Max fill fan-out (price-time / pro-rata) | 8 / 35 | 8 / 44 |
| Self-trade skips (price-time / pro-rata) | 6,564 / 7,961 | 6,507 / 8,032 |
| `state_hash` (price-time / pro-rata) | 7d8f570852562599 / 0ac25b855783070e | 8b74c1ea41c568fe / 6396454671e3e128 |

The fill counts and `state_hash` differ because self-trade handling changed (a self-trade no longer leaves a crossed remainder resting), so the two runs do not execute identical matching decisions on the same input. Cross-version `state_hash` values are not comparable.

What changed in the engine between the two runs: correctness fixes (self-trade handling, 64-bit per-level quantity accumulators, duplicate-id rejection, replace-to-zero as cancel, ingress and dataset validation); a size-class free-list allocator for book nodes in place of `std::pmr::unsynchronized_pool_resource`; a 64-byte-slot SPSC ring with a per-slot sequence word (ring 8 MiB to 4 MiB), cached peer indices, lazy next-level lookup in sweeps; a harness prefetch of the next dataset event outside the timed window; and harness hardening (in-band phase barriers so a trial ends only after its queue has drained, startup states so a failed affinity setup fails the run) that costs about 1.5 to 3.5 ns at P50 to P99. The performance changes were validated by interleaved A/B runs on the pinned cores during development (8 rounds per mode, faster in 8 of 8 rounds, identical state hash); those A/B logs are a development-session measurement and are not stored in the repository. The last three together measured price-time P50 420 to 320 ns and pro-rata 424 to 324 ns against the commit before them.

### Ablation baselines, earlier vs now

| Policy | Earlier ablation baseline P99.9 (per trial) | Now (per trial) | Median earlier | Median now |
|---|---|---|---|---|
| Price-Time | [1,698, 1,727, 1,835, 1,803, 1,762] | [1,157, 1,129, 1,143, 1,148, 1,155] | 1,762 | 1,148 |
| Pro-Rata | [3,019, 3,010, 3,331, 3,104, 3,065] | [2,329, 2,305, 2,386, 2,343, 2,440] | 3,065 | 2,343 |

### The earlier ablation data under the current verdict rule

The earlier run's published verdicts used the old P99-gated rule. Applying the current rule to the per-trial P99.9 values in `results/98a597b/ablation_*.csv` gives the following; this is a re-classification of old data, not a new measurement, and the earlier arms were built from the earlier engine. Ratio = median ablated / median baseline.

| Variant | Policy | Earlier ratio | Earlier verdict (current rule) | Now ratio | Now verdict |
|---|---|---|---|---|---|
| `mutex_queue` | Price-Time | 4.41x | PROVEN | 1.96x | PROVEN |
| `mutex_queue` | Pro-Rata | 2.54x | PROVEN | 1.44x | PROVEN |
| `unpinned` | Price-Time | 5.23x | PROVEN | 2.12x | PROVEN |
| `unpinned` | Pro-Rata | 1.60x | INCONCLUSIVE | 0.76x | ABLATED_FASTER |
| `malloc_pool` | Price-Time | 3.45x | PROVEN | 3.63x | PROVEN |
| `malloc_pool` | Pro-Rata | 2.27x | PROVEN | 1.85x | PROVEN |
| `flat_layout` | Price-Time | 1.02x | NO_MEASURABLE_EFFECT | 0.98x | NO_MEASURABLE_EFFECT |
| `flat_layout` | Pro-Rata | 1.16x | INCONCLUSIVE | 0.97x | NO_MEASURABLE_EFFECT |

Where the sessions disagree most is the `unpinned` row: in the earlier session the unpinned build was 5.23x worse in price-time (and 1.60x in pro-rata), in this one 2.12x and 0.76x. The earlier session's unpinned price-time arm had per-trial P99.9 values up to 144,646 ns; this session's reach 69,960 ns. The unpinned variant's result depends on where the scheduler puts it and how busy the desktop is, which is why that row is documented as isolation plus placement luck rather than as the effect of pinning.

The run of record is lower than the earlier run at every percentile in the table above. Part of that is the engine work listed here; part may be session-to-session variation (the same binary's tail percentiles can move 10 to 20% between sessions on this host). Only same-session interleaved comparisons are reliable at the 1 to 5% level, and this comparison is not one.

The earlier session also had two `--verbose` canonical runs (one per policy) that were not repeated for the run of record, so they have no counterpart here.

---

## Repeat runs of the same code

The production engine and harness code is identical in three full runs taken on 2026-11-12, all clean working tree, all 12 runs `VALID` with determinism `PASS` and zero dropped events:

- `results_new/run1_stamped_a27e77f/`: binaries report commit `a27e77f` because the commit stamp is taken when CMake configures, and CMake had not been reconfigured after the last commit. The only difference between `a27e77f` and `7c2fd2c` is a one-line formatting change in `tests/test_phase4_spsc.cpp`. Price-time canonical started 14:54 UTC. Host tuned with `scripts/tune_host.sh` for the two pinned cores.
- `results_new/7c2fd2c/`: commit `7c2fd2c`, same code, started 14:56 UTC, same tuning (two pinned cores).
- `results_new/77eb286/` (the run of record): started 15:31 UTC, the whole isolated cluster tuned with `sudo scripts/tune_host.sh 4,5,6,7`. The only code change against `7c2fd2c` is commit `77eb286`, which touches the ablation arms (next paragraph), not the production engine.

Median over 5 trials of the per-trial percentile (ns); sources: `results_new/run1_stamped_a27e77f/`, `results_new/7c2fd2c/`, `results_new/77eb286/` (`{price_time,pro_rata}.csv`, P99.99 from the raw files):

| Policy | Run | P50 | P90 | P99 | P99.9 | P99.99 (indicative) | P99.9 CV | state_hash |
|---|---|---|---|---|---|---|---|---|
| Price-Time | stamped `a27e77f` | 320 | 549 | 850 | 1,156 | 1,523 | 5.99% | `8b74c1ea41c568fe` |
| Price-Time | `7c2fd2c` | 321 | 551 | 856 | 1,158 | 1,707 | 11.99% | `8b74c1ea41c568fe` |
| Price-Time | `77eb286` (run of record) | 320 | 549 | 851 | 1,158 | 1,611 | 2.34% | `8b74c1ea41c568fe` |
| | **spread (max-min over median of the three)** | 0.3% | 0.4% | 0.7% | 0.2% | 11.4% | | |
| Pro-Rata | stamped `a27e77f` | 324 | 590 | 1,069 | 2,361 | 4,619 | 1.98% | `6396454671e3e128` |
| Pro-Rata | `7c2fd2c` | 325 | 598 | 1,082 | 2,380 | 4,648 | 2.11% | `6396454671e3e128` |
| Pro-Rata | `77eb286` (run of record) | 325 | 591 | 1,078 | 2,433 | 5,162 | 2.81% | `6396454671e3e128` |
| | **spread (max-min over median of the three)** | 0.3% | 1.4% | 1.2% | 3.0% | 11.7% | | |

Per-trial canonical P99.9 (ns), trial order 0 to 4:

| Policy | Stamped `a27e77f` | `7c2fd2c` | `77eb286` (record) |
|---|---|---|---|
| Price-Time | [1,313, 1,167, 1,150, 1,156, 1,147] | [1,478, 1,144, 1,122, 1,208, 1,158] | [1,149, 1,216, 1,155, 1,179, 1,158] |
| Pro-Rata | [2,414, 2,328, 2,412, 2,312, 2,361] | [2,418, 2,406, 2,380, 2,378, 2,290] | [2,543, 2,377, 2,433, 2,500, 2,403] |

The three canonical runs agree to within 1.4% at P50 to P99 and within 0.2% (price-time) and 3.0% (pro-rata) at P99.9; the `state_hash` is the same in all three (the same events and decisions). The indicative P99.99 moves more (11% and 12%), which is what a figure resting on about 9 samples per trial looks like. The per-trial P99.9 CV is higher in some runs (up to 12.0%, one outlier trial in the `7c2fd2c` price-time run) but every canonical run was STABLE. These are same-day repeats on one tuned host: they show the harness repeats minute to minute and across a tuning change; they do not show day-to-day stability, which has not been tested.

**Earlier ablation arms are superseded.** In the `run1_stamped_a27e77f` and `7c2fd2c` runs the ablation arms were not payload-matched: the flat-layout arm used the generic queue (price-time P50 388 ns against 321 for that run's baseline) and the mutex arm copied the whole `Order` through the queue. Their flat-layout and mutex-queue rows are therefore not comparable with the production arms or with the current ones, and commit `77eb286` replaced them (every arm now transports the same eight-field payload). Nothing from those two runs is re-classified here. Their baseline, unpinned and malloc-pool arms were not affected by the change; of those, only the unpinned row is quoted, and only to show how unstable it is:

| Run | Price-time unpinned / baseline P99.9 | Stored verdict | Pro-rata unpinned / baseline P99.9 | Stored verdict |
|---|---|---|---|---|
| stamped `a27e77f` | 0.85x | INCONCLUSIVE | 2.07x | INCONCLUSIVE |
| `7c2fd2c` | 1.24x | INCONCLUSIVE | 0.80x | ABLATED_FASTER |
| `77eb286` (run of record) | 2.12x | PROVEN | 0.76x | ABLATED_FASTER |

The verdicts in that table are the ones stored in each run's `ablation_summary.csv` (same rule); they are not re-derived. Across these three runs this row has read PROVEN, INCONCLUSIVE and ABLATED_FASTER: it measures isolation and placement on a busy desktop, so it should not be used as evidence for or against pinning.

---

## Source files

- Canonical: `results_new/77eb286/{price_time,pro_rata}.{csv,raw.csv,meta.json,console.txt}`
- Ablation: `results_new/77eb286/ablation_<variant>_<mode>.{csv,raw.csv,meta.json}`, `ablation_summary.{csv,md}`, `ablation_raw.jsonl`
- Repeat runs: `results_new/run1_stamped_a27e77f/` and `results_new/7c2fd2c/` (same layout)
- Earlier engine: `results/98a597b/` (frozen)
- `results/` and `results_new/` are gitignored; a fresh clone does not contain them.
