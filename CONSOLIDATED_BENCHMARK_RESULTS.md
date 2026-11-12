# Blitz LOB: Consolidated Benchmark Results

Per-trial results for every benchmark run in `results_new/7c2fd2c/` (the run of record) plus a repeat run and a comparison with an earlier engine. Every number here is computed from the saved `*.csv`, `*.raw.csv` and `*.meta.json` files; nothing was re-run to write this file, and each per-trial percentile was also recomputed from the raw samples and matches the CSV. Field and statistic definitions are in [METHODOLOGY.md](METHODOLOGY.md).

## Run of record

| Item | Value |
|---|---|
| Commit | `7c2fd2c`, working tree clean (`dirty: false` in every `meta.json`); ablation variants carry the commit string `7c2fd2c-ablation-<variant>` |
| Run date | 2026-11-12 (UTC; run ids carry the Unix start time 1794495382 = 14:56 UTC for the price-time canonical run) |
| Host | 12th Gen Intel Core i5-1235U (hybrid), kernel 7.0.0-43-generic, GCC 15.2.0, Release build |
| Boot parameters | `isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7` (cores 4-7 are one E-core cluster sharing a 2 MB L2; the preflight snapshot records `isolated=4-7`) |
| Pinned cores | producer on core 4, matching thread on core 5, `performance` governor on both, no SMT sibling on either, one NUMA node (`numactl --cpunodebind=0 --membind=0`, operator-bound, not self-verified) |
| Host tuning | `scripts/tune_host.sh` (device IRQs moved off the isolated cores, power-profiles-daemon masked, irqbalance stopped) |
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
- The canonical run and the ablation baseline are separate runs of the same binary at different times, so they differ slightly (price-time P99.9 1,158 vs 1,170). Ablation ratios are always computed against the ablation baseline, never the canonical run.
- Throughput is the paced input rate; `Max` is the single worst sample per trial and is dominated by host noise.
- `VALID`/`PASS` on every row is a result in itself. `state_hash` identical across trials shows deterministic replay of the same events, not that the matching is right.
- Limits: one host, one day, five trials per run, synthetic data, in-process latency (no network or market-data path). There is no repeat on a tuned host on another day; the repeat in [the repeat-run section](#repeat-run-same-code-stamped-a27e77f) was minutes apart in the same session.

## At a glance

Median over 5 trials of the per-trial percentile (ns). Source: `results_new/7c2fd2c/`.

| # | Run | P50 | P90 | P99 | P99.9 | Stability | Valid | Determinism |
|---|---|---|---|---|---|---|---|---|
| 1 | **Canonical Price-Time** | 321 | 551 | 856 | 1,158 | STABLE | YES | PASS |
| 2 | **Canonical Pro-Rata** | 325 | 598 | 1,082 | 2,380 | STABLE | YES | PASS |
| 3 | Ablation baseline Price-Time | 321 | 549 | 849 | 1,170 | STABLE | YES | PASS |
| 4 | Ablation baseline Pro-Rata | 324 | 591 | 1,081 | 2,500 | UNSTABLE | YES | PASS |
| 5 | Ablation mutex_queue Price-Time | 1,209 | 4,602 | 5,155 | 6,589 | STABLE | YES | PASS |
| 6 | Ablation mutex_queue Pro-Rata | 1,120 | 4,448 | 5,177 | 6,747 | STABLE | YES | PASS |
| 7 | Ablation unpinned Price-Time | 248 | 411 | 659 | 1,452 | UNSTABLE | YES | PASS |
| 8 | Ablation unpinned Pro-Rata | 252 | 444 | 824 | 1,996 | STABLE | YES | PASS |
| 9 | Ablation malloc_pool Price-Time | 347 | 597 | 1,428 | 4,427 | UNSTABLE | YES | PASS |
| 10 | Ablation malloc_pool Pro-Rata | 350 | 635 | 2,265 | 4,112 | STABLE | YES | PASS |
| 11 | Ablation flat_layout Price-Time | 388 | 623 | 931 | 1,236 | UNSTABLE | YES | PASS |
| 12 | Ablation flat_layout Pro-Rata | 395 | 665 | 1,152 | 2,362 | STABLE | YES | PASS |

- Canonical Price-Time P99.99 (indicative, recomputed from `price_time.raw.csv`): 1,707 ns.
- Canonical Pro-Rata P99.99 (indicative, recomputed from `pro_rata.raw.csv`): 4,648 ns.

---

## Canonical runs (the headline per matching policy)

### 1. Canonical Price-Time (FIFO)

- **Run ID**: `7c2fd2c-price_time-1794495382`  |  **Commit string**: `7c2fd2c`  |  **Working tree**: clean
- **Mode**: `price_time`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=7c2fd2c dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 321 | 551 | 883 | 1,478 | 374.2 | 13,706 | 84,991.6 |
| 1 | 319 | 547 | 846 | 1,144 | 368.7 | 6,775 | 84,991.7 |
| 2 | 320 | 547 | 842 | 1,122 | 369.3 | 9,344 | 84,991.7 |
| 3 | 321 | 552 | 859 | 1,208 | 372.0 | 9,596 | 84,991.6 |
| 4 | 321 | 552 | 856 | 1,158 | 372.1 | 9,984 | 84,991.7 |
| **Median** | **321** | **551** | **856** | **1,158** | 372.0 | 9,596 | 84,991.7 |
| CV across trials | 0.28% | 0.47% | 1.87% | 11.99% | 0.61% | 25.13% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 1.87%, P99.9 = 11.99%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,743 | non-crossing 49,375 | partial fills 4,895 | multi-level sweeps 19,117 | FOK orders 1,800 | self-trade skips 6,507  
Generated fills 64,420 | mean fills per crossing order 1.802 | max fill fan-out 8 | mean levels consumed 0.805

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `8b74c1ea41c568fe` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 2. Canonical Pro-Rata (proportional allocation)

- **Run ID**: `7c2fd2c-pro_rata-1794495390`  |  **Commit string**: `7c2fd2c`  |  **Working tree**: clean
- **Mode**: `pro_rata`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=7c2fd2c dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 325 | 595 | 1,082 | 2,418 | 391.1 | 9,085 | 84,991.9 |
| 1 | 324 | 596 | 1,076 | 2,406 | 390.4 | 9,534 | 84,991.8 |
| 2 | 325 | 598 | 1,082 | 2,380 | 391.0 | 8,857 | 84,991.8 |
| 3 | 325 | 599 | 1,082 | 2,378 | 392.1 | 11,068 | 84,991.8 |
| 4 | 326 | 599 | 1,081 | 2,290 | 391.7 | 7,948 | 84,991.7 |
| **Median** | **325** | **598** | **1,082** | **2,380** | 391.1 | 9,085 | 84,991.8 |
| CV across trials | 0.22% | 0.30% | 0.24% | 2.11% | 0.17% | 12.32% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 0.24%, P99.9 = 2.11%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,750 | non-crossing 49,368 | partial fills 4,901 | multi-level sweeps 19,154 | FOK orders 1,800 | self-trade skips 8,032  
Generated fills 74,817 | mean fills per crossing order 2.093 | max fill fan-out 44 | mean levels consumed 0.806

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `6396454671e3e128` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

- Price-Time order types from `price_time.console.txt`: GTC 79,185, IOC 4,133, FOK 1,800 (sum 85,118 = new orders). The console's stability verdict (STABLE) and CVs agree with the values above.
- Pro-Rata order types from `pro_rata.console.txt`: GTC 79,185, IOC 4,133, FOK 1,800 (sum 85,118 = new orders). The console's stability verdict (STABLE) and CVs agree with the values above.

Price-time trial 0 has a P99.9 of 1,478 ns against 1,122 to 1,208 ns in the other four trials; this single trial puts the P99.9 CV at 11.99%, close to the 15% STABLE threshold. The median (1,158) is not affected by it.

The two canonical policies replay the same 110,000 events; they differ only in how a crossing order is allocated. Pro-rata gives a floor share to nearly every eligible resting order, so one crossing order produces many more fills (max fan-out 44 against 8), which is why its P99 and P99.9 are higher. Each fill also costs about 37 ns in the harness's fill callback (two serialized timestamps and a digest fold), which is inside the timed window.

---

## Ablation suite (baseline + 4 variants, both policies)

Each variant is a separate build of the engine with one design decision reversed. All were run in one pass of `scripts/run_ablations.sh --trials 5` (`RESULTS_ROOT=results_new`).

### 3. Ablation baseline: all optimizations on, Price-Time

- **Run ID**: `7c2fd2c-price_time-1794495414`  |  **Commit string**: `7c2fd2c`  |  **Working tree**: clean
- **Mode**: `price_time`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=7c2fd2c dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 321 | 545 | 849 | 1,170 | 369.9 | 13,109 | 84,991.8 |
| 1 | 319 | 547 | 847 | 1,133 | 368.7 | 12,210 | 84,991.8 |
| 2 | 320 | 549 | 848 | 1,126 | 369.5 | 9,106 | 84,991.7 |
| 3 | 321 | 555 | 865 | 1,224 | 373.0 | 4,216 | 84,992.0 |
| 4 | 321 | 551 | 855 | 1,179 | 371.3 | 11,018 | 84,991.8 |
| **Median** | **321** | **549** | **849** | **1,170** | 369.9 | 11,018 | 84,991.8 |
| CV across trials | 0.28% | 0.70% | 0.88% | 3.39% | 0.46% | 35.53% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 0.88%, P99.9 = 3.39%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,743 | non-crossing 49,375 | partial fills 4,895 | multi-level sweeps 19,117 | FOK orders 1,800 | self-trade skips 6,507  
Generated fills 64,420 | mean fills per crossing order 1.802 | max fill fan-out 8 | mean levels consumed 0.805

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `8b74c1ea41c568fe` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 4. Ablation baseline: all optimizations on, Pro-Rata

- **Run ID**: `7c2fd2c-pro_rata-1794495454`  |  **Commit string**: `7c2fd2c`  |  **Working tree**: clean
- **Mode**: `pro_rata`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=7c2fd2c dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 324 | 589 | 1,073 | 2,442 | 392.8 | 86,768 | 84,991.9 |
| 1 | 323 | 591 | 1,072 | 2,359 | 388.3 | 11,605 | 84,991.8 |
| 2 | 324 | 591 | 1,088 | 2,500 | 390.5 | 10,356 | 84,991.8 |
| 3 | 325 | 593 | 1,081 | 2,587 | 391.8 | 10,618 | 84,991.9 |
| 4 | 332 | 607 | 1,224 | 3,691 | 410.3 | 23,845 | 84,991.6 |
| **Median** | **324** | **591** | **1,081** | **2,500** | 391.8 | 11,605 | 84,991.8 |
| CV across trials | 1.12% | 1.23% | 5.90% | 20.31% | 2.25% | 115.17% | 0.000% |

**Stability verdict**: UNSTABLE (CV of per-trial P99 = 5.90%, P99.9 = 20.31%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,750 | non-crossing 49,368 | partial fills 4,901 | multi-level sweeps 19,154 | FOK orders 1,800 | self-trade skips 8,032  
Generated fills 74,817 | mean fills per crossing order 2.093 | max fill fan-out 44 | mean levels consumed 0.806

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `6396454671e3e128` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 5. Ablation mutex_queue: lock-free SPSC ring replaced by std::mutex + std::queue, Price-Time

- **Run ID**: `7c2fd2c-ablation-mutex_queue-price_time-1794495422`  |  **Commit string**: `7c2fd2c-ablation-mutex_queue`  |  **Working tree**: clean
- **Mode**: `price_time`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=7c2fd2c-ablation-mutex_queue dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 1,106 | 1,785 | 5,080 | 6,432 | 1,253.4 | 16,529 | 84,992.2 |
| 1 | 1,143 | 2,870 | 5,108 | 6,469 | 1,425.0 | 12,723 | 84,992.3 |
| 2 | 1,209 | 4,602 | 5,155 | 6,589 | 1,735.6 | 11,901 | 84,992.3 |
| 3 | 1,242 | 4,636 | 5,208 | 6,800 | 1,883.3 | 12,268 | 84,992.4 |
| 4 | 1,267 | 4,688 | 5,395 | 6,889 | 2,024.8 | 11,419 | 84,992.3 |
| **Median** | **1,209** | **4,602** | **5,155** | **6,589** | 1,735.6 | 12,268 | 84,992.3 |
| CV across trials | 5.65% | 35.65% | 2.41% | 3.04% | 19.21% | 15.79% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 2.41%, P99.9 = 3.04%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,743 | non-crossing 49,375 | partial fills 4,895 | multi-level sweeps 19,117 | FOK orders 1,800 | self-trade skips 6,507  
Generated fills 64,420 | mean fills per crossing order 1.802 | max fill fan-out 8 | mean levels consumed 0.805

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `8b74c1ea41c568fe` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 6. Ablation mutex_queue: lock-free SPSC ring replaced by std::mutex + std::queue, Pro-Rata

- **Run ID**: `7c2fd2c-ablation-mutex_queue-pro_rata-1794495461`  |  **Commit string**: `7c2fd2c-ablation-mutex_queue`  |  **Working tree**: clean
- **Mode**: `pro_rata`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=7c2fd2c-ablation-mutex_queue dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 1,087 | 1,771 | 5,097 | 6,222 | 1,239.5 | 12,686 | 84,992.1 |
| 1 | 1,119 | 2,953 | 5,180 | 6,851 | 1,433.4 | 15,045 | 84,992.1 |
| 2 | 1,120 | 4,501 | 5,177 | 6,801 | 1,502.1 | 70,776 | 84,992.2 |
| 3 | 1,147 | 4,582 | 5,203 | 6,747 | 1,597.4 | 23,769 | 84,992.1 |
| 4 | 1,130 | 4,448 | 5,160 | 6,473 | 1,494.0 | 12,334 | 84,992.1 |
| **Median** | **1,120** | **4,448** | **5,177** | **6,747** | 1,494.0 | 15,045 | 84,992.1 |
| CV across trials | 1.95% | 34.23% | 0.78% | 4.01% | 9.16% | 92.67% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 0.78%, P99.9 = 4.01%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,750 | non-crossing 49,368 | partial fills 4,901 | multi-level sweeps 19,154 | FOK orders 1,800 | self-trade skips 8,032  
Generated fills 74,817 | mean fills per crossing order 2.093 | max fill fan-out 44 | mean levels consumed 0.806

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `6396454671e3e128` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 7. Ablation unpinned: thread pinning removed (OS-scheduled threads), Price-Time

- **Run ID**: `7c2fd2c-ablation-unpinned-price_time-1794495430`  |  **Commit string**: `7c2fd2c-ablation-unpinned`  |  **Working tree**: clean
- **Mode**: `price_time`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=7c2fd2c-ablation-unpinned dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 250 | 411 | 675 | 21,293 | 380.6 | 181,549 | 84,992.7 |
| 1 | 247 | 411 | 658 | 1,452 | 296.3 | 75,821 | 84,992.7 |
| 2 | 248 | 413 | 662 | 1,615 | 294.2 | 43,149 | 84,992.8 |
| 3 | 248 | 410 | 653 | 1,219 | 291.0 | 30,292 | 84,992.8 |
| 4 | 248 | 411 | 659 | 1,317 | 291.5 | 44,992 | 84,992.7 |
| **Median** | **248** | **411** | **659** | **1,452** | 294.2 | 44,992 | 84,992.7 |
| CV across trials | 0.44% | 0.27% | 1.25% | 165.40% | 12.59% | 82.20% | 0.000% |

**Stability verdict**: UNSTABLE (CV of per-trial P99 = 1.25%, P99.9 = 165.40%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,743 | non-crossing 49,375 | partial fills 4,895 | multi-level sweeps 19,117 | FOK orders 1,800 | self-trade skips 6,507  
Generated fills 64,420 | mean fills per crossing order 1.802 | max fill fan-out 8 | mean levels consumed 0.805

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `8b74c1ea41c568fe` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 8. Ablation unpinned: thread pinning removed (OS-scheduled threads), Pro-Rata

- **Run ID**: `7c2fd2c-ablation-unpinned-pro_rata-1794495469`  |  **Commit string**: `7c2fd2c-ablation-unpinned`  |  **Working tree**: clean
- **Mode**: `pro_rata`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=7c2fd2c-ablation-unpinned dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 252 | 443 | 828 | 2,106 | 305.3 | 63,717 | 84,992.7 |
| 1 | 250 | 442 | 815 | 1,897 | 300.9 | 37,759 | 84,992.7 |
| 2 | 251 | 444 | 824 | 1,996 | 302.5 | 10,507 | 84,992.7 |
| 3 | 252 | 445 | 816 | 1,905 | 309.5 | 72,641 | 84,992.7 |
| 4 | 255 | 458 | 914 | 2,167 | 311.8 | 18,234 | 84,992.7 |
| **Median** | **252** | **444** | **824** | **1,996** | 305.3 | 37,759 | 84,992.7 |
| CV across trials | 0.74% | 1.47% | 5.01% | 5.97% | 1.50% | 67.22% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 5.01%, P99.9 = 5.97%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,750 | non-crossing 49,368 | partial fills 4,901 | multi-level sweeps 19,154 | FOK orders 1,800 | self-trade skips 8,032  
Generated fills 74,817 | mean fills per crossing order 2.093 | max fill fan-out 44 | mean levels consumed 0.806

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `6396454671e3e128` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 9. Ablation malloc_pool: fixed object pools replaced by new/delete, Price-Time

- **Run ID**: `7c2fd2c-ablation-malloc_pool-price_time-1794495438`  |  **Commit string**: `7c2fd2c-ablation-malloc_pool`  |  **Working tree**: clean
- **Mode**: `price_time`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=7c2fd2c-ablation-malloc_pool dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 348 | 593 | 1,428 | 4,427 | 428.4 | 12,043 | 84,991.8 |
| 1 | 346 | 594 | 1,351 | 4,205 | 430.3 | 37,054 | 84,991.8 |
| 2 | 347 | 597 | 1,412 | 4,476 | 437.2 | 113,368 | 84,991.8 |
| 3 | 347 | 598 | 1,577 | 4,236 | 431.6 | 16,269 | 84,991.8 |
| 4 | 347 | 602 | 2,484 | 5,538 | 466.9 | 193,883 | 84,991.8 |
| **Median** | **347** | **597** | **1,428** | **4,427** | 431.6 | 37,054 | 84,991.8 |
| CV across trials | 0.20% | 0.60% | 28.68% | 12.02% | 3.65% | 104.92% | 0.000% |

**Stability verdict**: UNSTABLE (CV of per-trial P99 = 28.68%, P99.9 = 12.02%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,743 | non-crossing 49,375 | partial fills 4,895 | multi-level sweeps 19,117 | FOK orders 1,800 | self-trade skips 6,507  
Generated fills 64,420 | mean fills per crossing order 1.802 | max fill fan-out 8 | mean levels consumed 0.805

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `8b74c1ea41c568fe` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 10. Ablation malloc_pool: fixed object pools replaced by new/delete, Pro-Rata

- **Run ID**: `7c2fd2c-ablation-malloc_pool-pro_rata-1794495477`  |  **Commit string**: `7c2fd2c-ablation-malloc_pool`  |  **Working tree**: clean
- **Mode**: `pro_rata`  |  **Dataset**: `datasets/bench_110k.bin` (hash `51ce6285b274413e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=7c2fd2c-ablation-malloc_pool dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 350 | 630 | 2,230 | 4,165 | 445.5 | 11,212 | 84,991.6 |
| 1 | 350 | 632 | 2,283 | 4,053 | 446.7 | 14,380 | 84,991.8 |
| 2 | 351 | 635 | 2,213 | 4,059 | 450.6 | 67,974 | 84,991.9 |
| 3 | 350 | 635 | 2,265 | 4,112 | 452.9 | 86,768 | 84,991.8 |
| 4 | 351 | 635 | 2,381 | 4,449 | 484.4 | 254,431 | 84,991.8 |
| **Median** | **350** | **635** | **2,265** | **4,112** | 450.6 | 67,974 | 84,991.8 |
| CV across trials | 0.16% | 0.36% | 2.89% | 3.93% | 3.54% | 114.16% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 2.89%, P99.9 = 3.93%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,750 | non-crossing 49,368 | partial fills 4,901 | multi-level sweeps 19,154 | FOK orders 1,800 | self-trade skips 8,032  
Generated fills 74,817 | mean fills per crossing order 2.093 | max fill fan-out 44 | mean levels consumed 0.806

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `6396454671e3e128` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 11. Ablation flat_layout: hot/cold cache-line split removed (flat 88-byte Order), Price-Time

- **Run ID**: `7c2fd2c-ablation-flat_layout-price_time-1794495446`  |  **Commit string**: `7c2fd2c-ablation-flat_layout`  |  **Working tree**: clean
- **Mode**: `price_time`  |  **Dataset**: `results_new/7c2fd2c/ablation4_flat.bin` (hash `f330eff74ca2a47e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=7c2fd2c-ablation-flat_layout dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 375 | 611 | 977 | 2,598 | 435.1 | 18,510 | 84,992.0 |
| 1 | 384 | 622 | 957 | 1,977 | 441.8 | 19,851 | 84,992.0 |
| 2 | 388 | 624 | 928 | 1,233 | 440.2 | 5,780 | 84,991.9 |
| 3 | 389 | 623 | 925 | 1,221 | 440.6 | 11,997 | 84,991.9 |
| 4 | 389 | 625 | 931 | 1,236 | 441.8 | 11,225 | 84,991.9 |
| **Median** | **388** | **623** | **931** | **1,236** | 440.6 | 11,997 | 84,991.9 |
| CV across trials | 1.55% | 0.92% | 2.40% | 37.47% | 0.63% | 42.71% | 0.000% |

**Stability verdict**: UNSTABLE (CV of per-trial P99 = 2.40%, P99.9 = 37.47%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

**Workload (identical in every trial: yes)**: 100,000 measured events = 85,118 new orders + 14,882 cancels + 0 replaces (+ 10,000 warmup events not measured).  
Crossing orders 35,743 | non-crossing 49,375 | partial fills 4,895 | multi-level sweeps 19,117 | FOK orders 1,800 | self-trade skips 6,507  
Generated fills 64,420 | mean fills per crossing order 1.802 | max fill fan-out 8 | mean levels consumed 0.805

**Validity and determinism**: result `VALID` (valid column 1), determinism PASS, `state_hash` `8b74c1ea41c568fe` (identical across all trials), dropped events 0, arena fallback 0, pool exhaustion 0, unexpected allocations 0, reference validation `NOT_RUN`, smoketest flag `no`.

### 12. Ablation flat_layout: hot/cold cache-line split removed (flat 88-byte Order), Pro-Rata

- **Run ID**: `7c2fd2c-ablation-flat_layout-pro_rata-1794495485`  |  **Commit string**: `7c2fd2c-ablation-flat_layout`  |  **Working tree**: clean
- **Mode**: `pro_rata`  |  **Dataset**: `results_new/7c2fd2c/ablation4_flat.bin` (hash `f330eff74ca2a47e`)  |  **Env**: `isolated=4-7 rx_core=4 rx_governor=performance rx_smt_siblings=4 matching_core=5 matching_governor=performance matching_smt_siblings=5 git=7c2fd2c-ablation-flat_layout dirty=no`

| Trial | P50 | P90 | P99 | P99.9 | Mean | Max | Throughput (orders/s) |
|---|---|---|---|---|---|---|---|
| 0 | 394 | 663 | 1,147 | 2,429 | 461.2 | 8,926 | 84,992.0 |
| 1 | 393 | 664 | 1,155 | 2,432 | 460.8 | 10,247 | 84,991.9 |
| 2 | 395 | 665 | 1,151 | 2,356 | 462.2 | 10,171 | 84,992.2 |
| 3 | 396 | 667 | 1,152 | 2,343 | 462.9 | 8,411 | 84,992.1 |
| 4 | 396 | 667 | 1,153 | 2,362 | 464.0 | 9,818 | 84,992.0 |
| **Median** | **395** | **665** | **1,152** | **2,362** | 462.2 | 9,818 | 84,992.0 |
| CV across trials | 0.33% | 0.27% | 0.26% | 1.79% | 0.28% | 8.51% | 0.000% |

**Stability verdict**: STABLE (CV of per-trial P99 = 0.26%, P99.9 = 1.79%; STABLE needs both below 15%). Times are ns, end to end (`t1` to `t_match_end`), new orders only; the median row is the median of the per-trial values.

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

### Verdict inputs (run of record, `results_new/7c2fd2c/`)

Per-trial P99.9 in ns, trial order 0 to 4. Ratio = median ablated / median baseline. The verdicts below were recomputed here from the per-trial CSV values and agree with `ablation_summary.csv` and `ablation_summary.md` for all 8 cells.

#### Price-Time

Baseline per-trial P99.9: [1,170, 1,133, 1,126, 1,224, 1,179], median **1,170**, range 1,126 to 1,224.

| Design decision | Ablated per-trial P99.9 | Median | Range | Ratio | Median >= 110% (or <= 1/1.10)? | Trials fully separated? | Verdict |
|---|---|---|---|---|---|---|---|
| Lock-free SPSC queue (`mutex_queue`) | [6,432, 6,469, 6,589, 6,800, 6,889] | 6,589 | 6,432 to 6,889 | 5.63x | yes (slower) | yes | **PROVEN** |
| Core pinning + isolation (`unpinned`) | [21,293, 1,452, 1,615, 1,219, 1,317] | 1,452 | 1,219 to 21,293 | 1.24x | yes (slower) | no (ranges overlap) | **INCONCLUSIVE** |
| Fixed-slab object pooling (`malloc_pool`) | [4,427, 4,205, 4,476, 4,236, 5,538] | 4,427 | 4,205 to 5,538 | 3.78x | yes (slower) | yes | **PROVEN** |
| Cache-line hot/cold split (`flat_layout`) | [2,598, 1,977, 1,233, 1,221, 1,236] | 1,236 | 1,221 to 2,598 | 1.06x | no (within +-10%) | not needed | **NO_MEASURABLE_EFFECT** |

#### Pro-Rata

Baseline per-trial P99.9: [2,442, 2,359, 2,500, 2,587, 3,691], median **2,500**, range 2,359 to 3,691.

| Design decision | Ablated per-trial P99.9 | Median | Range | Ratio | Median >= 110% (or <= 1/1.10)? | Trials fully separated? | Verdict |
|---|---|---|---|---|---|---|---|
| Lock-free SPSC queue (`mutex_queue`) | [6,222, 6,851, 6,801, 6,747, 6,473] | 6,747 | 6,222 to 6,851 | 2.70x | yes (slower) | yes | **PROVEN** |
| Core pinning + isolation (`unpinned`) | [2,106, 1,897, 1,996, 1,905, 2,167] | 1,996 | 1,897 to 2,167 | 0.80x | yes (faster) | yes | **ABLATED_FASTER** |
| Fixed-slab object pooling (`malloc_pool`) | [4,165, 4,053, 4,059, 4,112, 4,449] | 4,112 | 4,053 to 4,449 | 1.64x | yes (slower) | yes | **PROVEN** |
| Cache-line hot/cold split (`flat_layout`) | [2,429, 2,432, 2,356, 2,343, 2,362] | 2,362 | 2,343 to 2,432 | 0.94x | no (within +-10%) | not needed | **NO_MEASURABLE_EFFECT** |

Reading the table: the mutex queue and the heap-allocated pools are PROVEN tail penalties in both policies. The hot/cold layout split shows no measurable effect (1.06x and 0.94x, inside the band); that is a not-detected result on this workload, and the reason is a footprint argument rather than a line-count one (see [DESIGN.md](DESIGN.md)). The `unpinned` row needs care: its price-time arm has one trial at 21,293 ns and four at 1,219 to 1,615 ns, which overlaps the baseline range, so INCONCLUSIVE; in pro-rata the unpinned build was faster (ABLATED_FASTER). In this session the OS most likely placed the unpinned threads on fast, quiet cores (its P50 is the lowest of all runs). The row therefore reflects isolation of the pinned cores plus placement luck on a busy desktop, not pinning in isolation, and it must not be read as a measured benefit or cost of pinning. Its lower P50 (248 and 252 ns against 321 and 324) comes from running on faster P-cores, not from unpinning being an advantage. The repeat-run section below shows the same binary swinging between 0.85x and 2.07x of baseline within one session.

The `flat_layout` rows carry a confound: the 64-byte-slot SPSC ring applies only to the 128-byte production `Order`, so the flat-layout build keeps the generic queue and pays its extra cost (its P50 is 388 and 395 ns against 321 and 324). The P99.9 comparison is therefore layout plus queue, and still lands inside the +-10% band. The pro-rata baseline arm has one 3,691 ns trial among four at 2,359 to 2,587 ns (hence its UNSTABLE label); the median is not affected.

`meta.json` for the unpinned variant still prints configured cores 4 and 5: the preflight checks the configured cores, not the actual affinity mask.

---

## Comparison with the earlier run (98a597b)

> **This is not a controlled A/B.** `results/98a597b/` is an earlier engine (before the correctness fixes and the performance work listed below) measured in an earlier session, on the same host and dataset. Hash `51ce6285b274413e` is the same dataset, but the engine, the build, the day and the host condition all differ. Treat the numbers as informative; they do not isolate any single change.

Both sets: median over 5 trials of the per-trial percentile (ns), new orders only. P99.99 is recomputed from the raw samples with the snippet in [METHODOLOGY.md](METHODOLOGY.md#recomputing-a-percentile-from-raw-samples) and is indicative only (about 9 samples per trial). Sources: `results/98a597b/{price_time,pro_rata}.csv` and `results_new/7c2fd2c/{price_time,pro_rata}.csv`.

| Policy | Run | P50 | P90 | P99 | P99.9 | P99.99 (indicative) |
|---|---|---|---|---|---|---|
| Price-Time | earlier, `98a597b` | 485 | 786 | 1,225 | 1,850 | 8,282 |
| Price-Time | run of record, `7c2fd2c` | 321 | 551 | 856 | 1,158 | 1,707 |
| | **new / earlier** | 0.66x | 0.70x | 0.70x | 0.63x | 0.21x |
| Pro-Rata | earlier, `98a597b` | 491 | 826 | 1,425 | 3,132 | 9,708 |
| Pro-Rata | run of record, `7c2fd2c` | 325 | 598 | 1,082 | 2,380 | 4,648 |
| | **new / earlier** | 0.66x | 0.72x | 0.76x | 0.76x | 0.48x |

What also changed between the two canonical runs (same dataset, so these differences come from the engine):

| Item | Earlier `98a597b` | `7c2fd2c` |
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
| Price-Time | [1,698, 1,727, 1,835, 1,803, 1,762] | [1,170, 1,133, 1,126, 1,224, 1,179] | 1,762 | 1,170 |
| Pro-Rata | [3,019, 3,010, 3,331, 3,104, 3,065] | [2,442, 2,359, 2,500, 2,587, 3,691] | 3,065 | 2,500 |

### The earlier ablation data under the current verdict rule

The earlier run's published verdicts used the old P99-gated rule. Applying the current rule to the per-trial P99.9 values in `results/98a597b/ablation_*.csv` gives the following; this is a re-classification of old data, not a new measurement. Ratio = median ablated / median baseline.

| Variant | Policy | Earlier ratio | Earlier verdict (current rule) | Now ratio | Now verdict |
|---|---|---|---|---|---|
| `mutex_queue` | Price-Time | 4.41x | PROVEN | 5.63x | PROVEN |
| `mutex_queue` | Pro-Rata | 2.54x | PROVEN | 2.70x | PROVEN |
| `unpinned` | Price-Time | 5.23x | PROVEN | 1.24x | INCONCLUSIVE |
| `unpinned` | Pro-Rata | 1.60x | INCONCLUSIVE | 0.80x | ABLATED_FASTER |
| `malloc_pool` | Price-Time | 3.45x | PROVEN | 3.78x | PROVEN |
| `malloc_pool` | Pro-Rata | 2.27x | PROVEN | 1.64x | PROVEN |
| `flat_layout` | Price-Time | 1.02x | NO_MEASURABLE_EFFECT | 1.06x | NO_MEASURABLE_EFFECT |
| `flat_layout` | Pro-Rata | 1.16x | INCONCLUSIVE | 0.94x | NO_MEASURABLE_EFFECT |

Where the two sessions disagree most is the `unpinned` row: in the earlier session the unpinned build was 5.23x worse in price-time (and 1.60x in pro-rata), in this session 1.24x and 0.80x. The earlier session's unpinned price-time arm had per-trial P99.9 values up to 144,646 ns; this session's had one large trial (21,293 ns). The unpinned variant's result depends on where the scheduler puts it and how busy the desktop is, which is why that row is documented as isolation plus placement luck rather than as the effect of pinning.

The run of record is lower than the earlier run at every percentile in the table above. Part of that is the engine work listed here; part may be session-to-session variation (the same binary's tail percentiles can move 10 to 20% between sessions on this host). Only same-session interleaved comparisons are reliable at the 1 to 5% level, and this comparison is not one.

The earlier session also had two `--verbose` canonical runs (one per policy) that were not repeated for the run of record, so they have no counterpart here.

---

## Repeat run (same code, stamped a27e77f)

`results_new/run1_stamped_a27e77f/` is a first full run (canonical plus ablation suite) of the same source as the run of record, taken minutes earlier in the same session. Its binaries report commit `a27e77f` because the commit stamp is taken when CMake configures, and CMake had not been reconfigured after the last commit. The only difference between `a27e77f` and `7c2fd2c` is a one-line formatting change in `tests/test_phase4_spsc.cpp`, so the engine and harness code are identical. Run ids carry Unix start time 1794495250 = 14:54 UTC. Working tree clean, all 12 runs `VALID`, determinism `PASS`, zero dropped events.

Median over 5 trials of the per-trial percentile (ns); source `results_new/run1_stamped_a27e77f/` and `results_new/7c2fd2c/`:

| Policy | Run | P50 | P90 | P99 | P99.9 | P99.99 (indicative) | state_hash |
|---|---|---|---|---|---|---|---|
| Price-Time | repeat, stamped `a27e77f` | 320 | 549 | 850 | 1,156 | 1,523 | `8b74c1ea41c568fe` |
| Price-Time | run of record, `7c2fd2c` | 321 | 551 | 856 | 1,158 | 1,707 | `8b74c1ea41c568fe` |
| | **difference** | -0.3% | -0.4% | -0.7% | -0.2% | -10.8% | |
| Pro-Rata | repeat, stamped `a27e77f` | 324 | 590 | 1,069 | 2,361 | 4,619 | `6396454671e3e128` |
| Pro-Rata | run of record, `7c2fd2c` | 325 | 598 | 1,082 | 2,380 | 4,648 | `6396454671e3e128` |
| | **difference** | -0.3% | -1.3% | -1.2% | -0.8% | -0.6% | |

Per-trial canonical P99.9 (ns), trial order 0 to 4:

| Policy | Repeat | Run of record |
|---|---|---|
| Price-Time | [1,313, 1,167, 1,150, 1,156, 1,147] | [1,478, 1,144, 1,122, 1,208, 1,158] |
| Pro-Rata | [2,414, 2,328, 2,412, 2,312, 2,361] | [2,418, 2,406, 2,380, 2,378, 2,290] |

Ablation verdicts, repeat vs run of record (same rule, per-trial P99.9; ratios against each run's own baseline):

| Variant | Policy | Repeat ratio | Repeat verdict | Run-of-record ratio | Run-of-record verdict |
|---|---|---|---|---|---|
| `mutex_queue` | Price-Time | 5.84x | PROVEN | 5.63x | PROVEN |
| `mutex_queue` | Pro-Rata | 2.86x | PROVEN | 2.70x | PROVEN |
| `unpinned` | Price-Time | 0.85x | INCONCLUSIVE | 1.24x | INCONCLUSIVE |
| `unpinned` | Pro-Rata | 2.07x | INCONCLUSIVE | 0.80x | ABLATED_FASTER |
| `malloc_pool` | Price-Time | 3.56x | PROVEN | 3.78x | PROVEN |
| `malloc_pool` | Pro-Rata | 1.71x | PROVEN | 1.64x | PROVEN |
| `flat_layout` | Price-Time | 1.07x | NO_MEASURABLE_EFFECT | 1.06x | NO_MEASURABLE_EFFECT |
| `flat_layout` | Pro-Rata | 1.08x | NO_MEASURABLE_EFFECT | 0.94x | NO_MEASURABLE_EFFECT |

Ablation baseline median P99.9 (ns): Price-Time repeat 1,135 vs record 1,170; Pro-Rata repeat 2,342 vs record 2,500.

Canonical P50 to P99.9 agree to within 1.3% across the two runs, and 7 of the 8 ablation verdicts are identical; the one that differs is the `unpinned` pro-rata row (INCONCLUSIVE at 2.07x in the repeat, ABLATED_FASTER at 0.80x in the run of record), which is the placement-sensitive row discussed above. The P99.99 of the price-time runs differs by 10.8% (1,523 vs 1,707), which is what an indicative figure resting on about 9 samples per trial looks like. This is a same-session repeat: it shows the harness is repeatable minute to minute; it does not show day-to-day stability of the host, which has not been tested on a tuned host.

---

## Source files

- Canonical: `results_new/7c2fd2c/{price_time,pro_rata}.{csv,raw.csv,meta.json,console.txt}`
- Ablation: `results_new/7c2fd2c/ablation_<variant>_<mode>.{csv,raw.csv,meta.json}`, `ablation_summary.{csv,md}`, `ablation_raw.jsonl`
- Repeat: `results_new/run1_stamped_a27e77f/` (same layout)
- Earlier engine: `results/98a597b/` (frozen)
- `results/` and `results_new/` are gitignored; a fresh clone does not contain them.
