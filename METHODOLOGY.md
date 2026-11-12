# Measurement Methodology

Latency is the time from the producer's timestamp, taken just before `push()`, to the end of matching, measured in-process on a synthetic stream replayed open loop between two pinned threads on isolated cores. A run is 5 trials; the headline is the median of per-trial percentiles. The harness refuses to run on an untuned host and marks a run invalid on drops, pool exhaustion, clock fallback or non-repeatable state. It is not network or market-data latency, and the harness's own timestamps sit inside the window (Threats to validity). Components: [ARCHITECTURE](ARCHITECTURE.md). Design evidence: [DESIGN](DESIGN.md).

| Environment of record | |
|---|---|
| Commit / date | `77eb286`, clean tree, 2026-11-12. Data: `results_new/77eb286/` (local, gitignored) |
| Host | i5-1235U (hybrid), Linux 7.0.0-43-generic, GCC 15.2.0, Release; clock `rdtscp_calibrated`, ~30.4 ns per stamp |
| Cores / boot | Producer core 4, matcher core 5 (cores 4-7: one E-core cluster, shared 2 MB L2); `isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7`; no SMT sibling, one NUMA node (`numactl`). Whole isolated cluster tuned with `sudo scripts/tune_host.sh 4,5,6,7`: `performance` governor and energy preference on cores 4-7, NMI watchdog off, `power-profiles-daemon` masked, `irqbalance` stopped, movable IRQs moved off (only NVMe queue IRQs remain, which fire on disk I/O) |
| Dataset | `datasets/bench_110k.bin` (seed 42, 100 kHz, hash `51ce6285b274413e`, [`manifest.txt`](datasets/manifest.txt)) |
| Per trial | 10,000 warmup + 100,000 measured events, fresh book and pools: 85,118 new orders (the samples), 14,882 cancels, 0 replaces |

## What is measured

```
producer: spin to arrival -> t1 -> push() ... ring ... t_pop_start -> pop() -> t_pop_end
matcher:  -> t_match_start -> match() [per fill: stamp, callback, stamp] -> t_match_end
```

| Field (raw CSV) | Definition |
|---|---|
| `queue_residence_ns` | `t_pop_start` minus `t1`: the producer's `push()` plus any wait in the ring |
| `queue_pop_ns` | `t_pop_end` minus `t_pop_start` (the successful `pop()`) |
| `fill_publish_ns` | Fill-callback time summed over the event's fills (one stamp pair per fill) |
| `match_time_ns` | `t_match_end` minus `t_match_start` minus `fill_publish_ns` (cancels: `cancel_order()` time) |
| `end_to_end_ns` | `t_match_end` minus `t1`. Every "ns per order" figure in these docs is this field |

- **Clock**: serialized `RDTSCP` (`lfence` both sides), scaled by a one-time calibration against `steady_clock` (two 1 s samples; more than 0.5% disagreement or a core migration falls back and invalidates the run). Stamp overhead is measured per run and not subtracted.
- **Open-loop pacing**: each target is `phase_start + (arrival_offset - first_offset)`; the producer spins to it and never waits for earlier events, so a slow event does not delay later arrivals. `t1` is the actual spin exit, so producer lateness is not charged (max drift is printed, not gated). Queueing behind a slow event is charged: 4.7% of events arrive while the previous one is still in flight (their queue residence p50 is about 279 ns).
- **Back-pressure**: a full ring is retried 1,000 times, then the event is dropped, counted and the run invalid.
- **Phase barriers**: an in-band `PHASE_BARRIER` event follows the warmup and the measured replay. The matcher acknowledges it only after all earlier events finish, so the sample sink attaches after warmup completed and a trial ends only after its queue drained. It is not counted as an event or sample.
- **Worker startup**: the matcher reports `NOT_STARTED/STARTING/READY/FAILED`; a failed affinity setup fails the run instead of hanging. Barriers plus startup states cost +1.5 to 3.5 ns at P50-P99 (development A/B, not stored).
- **Population**: new orders only; cancels are in the raw file (`is_cancel=1`) but excluded from the headline. Offered load is 85,000 new orders/s, not capacity; saturation was not measured.

## Statistics

| Item | Definition |
|---|---|
| Percentile | Sort the trial's samples, take index `floor(p/100 * N)` clamped to `N-1`; no interpolation |
| Reported figure | Median across the 5 trials of each per-trial percentile (`include/hydra/stats.hpp`) |
| STABLE | CV of per-trial P99 and P99.9 both below 15% (descriptive). Under 5 trials: `[SMOKETEST]`, not citable |
| P99.99 | Not in the CSVs; recomputed from raw samples; indicative only (about 9 samples per trial lie above it) |

### Recomputing a percentile from raw samples

```python
import csv, statistics
t = {}
for r in csv.DictReader(open("results_new/77eb286/price_time.raw.csv", newline="")):
    if r["is_cancel"] != "1" and r["is_replace"] != "1":
        t.setdefault(int(r["trial"]), []).append(int(r["end_to_end_ns"]))
pct = lambda v, p: sorted(v)[min(int(p / 100 * len(v)), len(v) - 1)]
for p in (99.9, 99.99):
    print(p, statistics.median(pct(v, p) for v in t.values()))
```

This prints 1,158 (P99.9) and 1,611 (P99.99, indicative) ns for price-time. For cancel latency keep `is_cancel == "1"`. Trial P99.9 values: 1,149 / 1,216 / 1,155 / 1,179 / 1,158 (CV 2.3%).

## Validity gate

| Check (post-trial failures still write files, with an `invalid_reason`) | When | Outcome |
|---|---|---|
| Both cores in `/sys/devices/system/cpu/isolated`, `performance` governor, no SMT sibling | Preflight | Refuse to run |
| Dropped event, arena fallback to the heap, or order/level pool exhaustion | After trials | Invalid |
| Clock fell back to `steady_clock`; `state_hash` differs between trials | After trials | Invalid |
| Dirty tree at build time without `--allow-dirty` | After trials | Invalid |

Preflight does not check `nohz_full`, `rcu_nocbs`, IRQ placement, NUMA binding, background load, or the mask the threads actually run on ([ALL_COMMANDS](ALL_COMMANDS.md)). `state_hash` folds each fill's maker, taker, price and quantity: equal across trials means the replay is repeatable. It is a determinism fingerprint, not a correctness oracle; `reference_validation_status` is `NOT_RUN`.

## Ablation verdicts

`scripts/run_ablations.sh` runs a baseline and four variants in both policies. A verdict compares per-trial P99.9 (5 per arm) of a variant with the baseline from the same session. `scripts/ablation_verdict.py` and `hydra::stats::classify_ablation` implement one rule; both run the same 53-case fixture `tests/verdict_cases.json` (`tests/test_phase10_verdict.cpp`, `tests/test_verdict_parity.py`). First match wins:

| Verdict | Condition |
|---|---|
| INVALID | Failed validity gate, empty arm, or baseline median <= 0 |
| INCONCLUSIVE | Fewer than 5 trials in either arm |
| NO_MEASURABLE_EFFECT | Median ablated P99.9 within +-10% of baseline (integer-percent test) |
| PROVEN | Median ablated >= 110% of baseline AND every ablated trial above every baseline trial (chance 1/252, 5 v 5) |
| ABLATED_FASTER | Mirror of PROVEN (formerly `REGRESSION`) |
| INCONCLUSIVE | A material median gap whose per-trial ranges overlap |

Worked example (price-time, ablation baseline trials 1,129-1,157, median 1,148): the mutex queue's trials are 2,172-2,287, all above 1,157, median 1.96x: PROVEN, and with a gap of about 1,000 ns between the ranges it is a robust row (P50 is 2.7x worse, 861 vs 320 ns). Pro-rata malloc pool (trials 4,068-4,425 vs baseline 2,305-2,440, median 1.85x) is PROVEN the same way. Pro-rata unpinned (median 1,782 vs 2,343, every trial 1,720-1,861 below 2,305) is ABLATED_FASTER.

A PROVEN verdict can still be weak: price-time unpinned reads PROVEN (median 2,435, 2.12x) only because its fastest trial, 1,161, is 4 ns above the slowest baseline trial, 1,157, while its five trials span 1,161-69,960. That row deserves little weight: the rule counts a 4 ns separation the same as a 1,000 ns one, the median is one trial in a bimodal spread, and the same row has read INCONCLUSIVE, INCONCLUSIVE/ABLATED_FASTER and PROVEN/ABLATED_FASTER across sessions as desktop load changed. It measures isolation and placement luck, not pinning ([DESIGN](DESIGN.md#core-isolation-and-pinning)).

Limits: 5 trials per arm; the 10% band is a convention; NO_MEASURABLE_EFFECT means not detected, not equal; one outlier trial can hide behind an in-band median; one host, one session. The earlier rule gated on P99 and a CV noise floor, hid a 40x tail penalty and flipped on noise, so it was replaced. Verdicts use the ablation's own baseline run (P99.9 1,148 price-time, 2,343 pro-rata), never the canonical run (1,158, 2,433); the flat-layout variant replays its own dataset. Every arm sends the same eight-field payload through its queue, so each ablation changes one thing.

## Output files

For `--output X`: `X.csv` (one row per trial), `X.raw.csv` (one row per measured event: timing fields, fill counts, `is_cancel`, `is_replace`), `X.meta.json` (commit, dirty flags, dataset hash, CPU, clock, validity). Contents of `results_new/77eb286/` (run logs are one level up):

| Files | Contents |
|---|---|
| `price_time.*`, `pro_rata.*` (`.csv`, `.raw.csv`, `.meta.json`, `.console.txt`) | Canonical runs and console reports |
| `ablation_<variant>_<mode>.*` | `baseline`, `mutex_queue`, `unpinned`, `malloc_pool`, `flat_layout`, both modes |
| `ablation_summary.{csv,md}`, `ablation_raw.jsonl` | Verdict table, medians, per-trial P99.9 inputs |
| `ablation4_flat.bin` | The flat-layout variant's own 88-byte-`Order` dataset |

Gitignored and regenerated by [ALL_COMMANDS](ALL_COMMANDS.md). The frozen `results/` holds an older run (`98a597b`).

## Threats to validity

- **Harness cost in the window**: each fill callback costs about 37 ns (two serialized stamps plus digest fold); at about 21 fills in a P99.9 pro-rata event that is roughly 21% of the figure. Three or four stamps per event also sit inside the window. Neither is subtracted.
- **One host, one day, 5 trials**: no repeat on another day. The same production code measured three times on the tuned host agreed within about 1-2% at P50-P99.9, but tails of other sessions have moved 10-20%; only same-session interleaved comparisons resolve 1-5%.
- **Synthetic data**: about 80% of generated cancels target already-filled or cancelled IDs (only ~3% of events are real cancels), and the book grows from ~1,200 to ~14,300 resting orders without reaching steady state.
- **No correctness oracle**: reference validation is `NOT_RUN`; there is no independent matcher in the repo. Producer lateness is not charged (pacing above).
- **`queue_residence_ns` wrap rows**: 55 of 1,000,000 raw rows (23 price-time, 32 pro-rata; 22 and 28 among new orders) have a pop attempt that began before the producer stamp, so the unsigned subtraction wraps. `end_to_end_ns` is unaffected; clamp first.
- **Development A/B evidence** for the allocator and ring-slot changes (8 interleaved paired rounds) is not stored in the repo; the ablations of the run of record are.
- **CI** (`.github/workflows/format.yml`) only checks formatting (clang-format, black); it does not run tests.
