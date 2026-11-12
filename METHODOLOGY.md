# Measurement Methodology

Latency is the time from the producer's timestamp, taken just before `push()`, to the end of matching, measured
in-process on a synthetic stream replayed open loop between two pinned threads. A run is 5 trials, reported as the
median of per-trial percentiles. The harness refuses to run on an untuned host and marks a run invalid on drops,
pool exhaustion, clock fallback or non-repeatable state. Figures predate the matching fixes ([README](README.md)). Components: [ARCHITECTURE](ARCHITECTURE.md). Design evidence: [DESIGN](DESIGN.md).

| Environment of record | |
|---|---|
| Commit / date | `98a597b` (clean tree), 2026-07-21 |
| Host / toolchain | Intel i5-1235U, cores 4 (producer) and 5 (matcher) isolated, `performance` governor, no SMT sibling, one NUMA node; GCC 15.2.0, Release |
| Dataset / trial | `datasets/bench_110k.bin` (seed 42, 100 kHz, [`manifest.txt`](datasets/manifest.txt)); per trial 10,000 warmup + 100,000 measured events on a fresh book and pools, 85,118 new-order samples |

## What is measured

Timeline: `t0` (producer stamp), `t_pop_start`, `t_pop_end`, `t_match_start`, `t_match_end` (all RDTSCP).

| Field | Definition |
|---|---|
| `queue_residence_ns` | `t_pop_start` minus `t0`. Includes the producer's `push()` call and any wait in the ring. |
| `queue_pop_ns`, `fill_publish_ns` | Duration of the successful `pop()`; time in the fill callback, summed over the event's fills. |
| `match_time_ns` | `match()` or `replace()` time minus `fill_publish_ns`; for cancels, `cancel_order()` time. |
| `end_to_end_ns` | `t_match_end` minus `t0`. Every "ns per order" figure in these docs is this field. |

- **Clock**: serialized `RDTSCP` (`lfence` on both sides), scaled by a one-time calibration against `steady_clock`
  (two 1 s samples). More than 0.5% disagreement or a core migration falls back to `steady_clock` and invalidates
  the run. Timestamp overhead is about 34-35 ns per pair, recorded per trial and not subtracted.
- **Pacing**: each event's target is `phase_start + (arrival_offset_ns - first_offset)`; the producer spins on the
  TSC until then and never waits for earlier events to finish, so a slow match does not delay later arrivals (the
  coordinated-omission failure mode). `t0` is read after the spin, i.e. the actual release time: if the producer
  itself runs late, that lateness is not added to latency. The worst lag is printed as max schedule drift (not gated).
- **Back-pressure**: a full ring is retried 1,000 times, then the event is dropped, counted and the run invalidated.
- **Phase completion**: an in-band control marker follows each warmup and measured replay. The matcher acknowledges
  it only after all earlier queue events finish, so warmup samples cannot leak into measurement and the book is
  drained before a trial reset. The marker is excluded from event counts and latency samples.
- **Population**: new orders only (cancels are in the raw file, excluded; the dataset has no replaces). 84,992
  orders/s is the paced new-order rate (100 kHz, about 85% new), not capacity.

## Statistics

| Item | Definition |
|---|---|
| Percentile | Sort the trial's samples, take index `floor(p/100 * N)` clamped to `N-1`. No interpolation. |
| Reported figure | Median across trials of each per-trial percentile. Spread is max minus min of per-trial P99.9. |
| STABLE label | CV of per-trial P99 and P99.9 both below 15% (descriptive, not a test). Fewer than 5 trials is flagged `[SMOKETEST]`, never citable. |
| P99.99 | Not in the CSVs (P50, P90, P99, P99.9, max, mean only). Recomputed from raw samples; indicative only, about 9 samples per trial lie above it. |

### Recomputing a percentile from raw samples

```python
import csv, statistics
t = {}
for r in csv.DictReader(open("results/<hash>/price_time.raw.csv", newline="")):
    if r["is_cancel"] != "1" and r["is_replace"] != "1":
        t.setdefault(int(r["trial"]), []).append(int(r["end_to_end_ns"]))
pct = lambda v, p: sorted(v)[min(int(p / 100 * len(v)), len(v) - 1)]
for p in (99.9, 99.99):
    print(p, statistics.median(pct(v, p) for v in t.values()))
```

For the `98a597b` price-time file this gives 1,850 ns and 8,282 ns. For cancel latency keep `is_cancel == "1"`.

## Validity gate

| Check (post-trial failures still write files, with an `invalid_reason`) | When | Outcome |
|---|---|---|
| Both cores in `/sys/devices/system/cpu/isolated`, `performance` governor, no SMT sibling | Preflight | Refuse to run |
| Dropped event, arena fallback to the heap, or order/level pool exhaustion | After trials | Invalid |
| Clock fell back to `steady_clock`; or `state_hash` differs between trials | After trials | Invalid |
| Dirty tree at build time without `--allow-dirty` | After trials | Invalid |

Preflight is per core. It does not check `nohz_full`, `rcu_nocbs`, IRQ placement, NUMA binding or background
load; the operator does (setup in [ALL_COMMANDS](ALL_COMMANDS.md)).

**Determinism fingerprint.** `state_hash` folds logical counters and each fill's maker, taker, price and quantity
(no timing). Equal hashes across fresh-state trials show the replay is repeatable. It is not a correctness oracle:
summary CSVs report `reference_validation_status=NOT_RUN`.

## Ablation verdicts

`scripts/run_ablations.sh` runs a baseline and four variants under both modes (10 runs) and compares each variant's
median P99 and P99.9 with the baseline from the same session, mirroring `hydra::stats::classify_ablation` (the
reference for verdict logic). With `delta = ablated - baseline`, the first rule that applies wins:

| Verdict | Condition |
|---|---|
| INVALID | A run is missing, invalid, nondeterministic, dropped events, or exhausted a resource |
| REGRESSION | Both deltas negative |
| PROVEN | `delta_P99 > max(CV_base, CV_abl) * base_P99` (CV of per-trial P99) and `delta_P99.9 > 0` |
| NOT PROVEN | Both deltas nonpositive |
| INCONCLUSIVE | Anything else |

Worked example (`98a597b`, price-time, unpinned): P99.9 rises 1,762 to 9,216 ns (5.23x) but P99 falls 1,222 to
1,098 ns, so INCONCLUSIVE; pro-rata is PROVEN. Verdicts use the ablation-baseline run, never the canonical run;
the flat-layout variant replays its own dataset. Results: [README](README.md), [DESIGN](DESIGN.md).

## Output files

| File | Contents |
|---|---|
| `X.csv` | One row per trial: percentiles, throughput, workload and resource counters, `state_hash`, validity, smoketest flag |
| `X.raw.csv` | One row per measured event: the timing fields above, fill counts, `is_cancel`, `is_replace` |
| `X.meta.json` | Commit, dirty flags, mode, dataset path and hash, CPU, clock source, compiler, host snapshot, validity and reason |
| `ablation_summary.{csv,md}` | Per-variant medians, ratios and verdicts (ablation suite only) |

`X` is the `--output` path. `results/` is gitignored; per-trial values: [CONSOLIDATED_BENCHMARK_RESULTS](CONSOLIDATED_BENCHMARK_RESULTS.md).

## Threats to validity

- Figures predate the matching fixes; no tuned-host re-measure of the current engine exists.
- In-process latency on synthetic data (no network or exchange path); no independent reference matcher.
- One x86-64 host (hybrid CPU), one session, 5 trials per run; P99.99 is indicative.
