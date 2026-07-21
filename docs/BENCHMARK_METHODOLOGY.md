# Blitz LOB — Benchmark Methodology

This document is the precise, load-bearing definition of every number the
redesigned benchmark harness (`src/benchmark.cpp`, `include/hydra/stats.hpp`,
`scripts/run_ablations.sh`) produces. If a claim made anywhere else (README,
a resume, a conversation) can't be traced to a definition in this document,
don't make the claim.

## 1. What the benchmark measures

`run_benchmark()` (`src/benchmark.cpp`) drives the **real production path**:
the same `matching_thread_fn` (`src/pipeline.cpp`) the live pipeline uses,
popping from the real lock-free `SpscQueue`, calling the real
`Matcher::match()`/`Matcher::replace()`, mutating the real `OrderBook`. The
only benchmark-specific code is the producer-side pacing loop
(`replay_phase`) and the `RawSampleSink` recorder. Benchmark mode never
bypasses production matching/book/queue code.

## 2. Measurement categories and exact boundaries

| Category | Start | End | Where |
|---|---|---|---|
| Queue push latency | Immediately before the successful `SpscQueue::push()` call | Immediately after it returns `true` | `replay_phase()`, reported as a trial-level **mean/max only** (see §2a) |
| Queue residence time | `order.timestamp_ns` (producer's intended-arrival timestamp, stamped after the open-loop pacing wait) | `t_pop_start` (consumer, immediately before the successful `pop()` call) | `LatencySample::queue_residence_ns` |
| Queue pop latency | `t_pop_start` | `t_pop_end` (pop returns) | `LatencySample::queue_pop_ns` |
| Matching-engine processing latency | `t_match_start` (immediately before `Matcher::match()`/`replace()`) | `t_match_end` (immediately after), **minus** `fill_publish_ns` | `LatencySample::match_time_ns` |
| Fill-event publication latency | Entry to the `on_fill` callback | Exit from the `on_fill` callback, summed across every fill one order generated | `LatencySample::fill_publish_ns` — always measured (not flag-gated); cost is one serialized RDTSCP pair per generated fill, paid only on crossing orders |
| End-to-end ingress-to-completion latency | `order.timestamp_ns` | `t_match_end` | `LatencySample::end_to_end_ns` — **this is what "ns/order" in every report means** |
| Batch completion time | Wall-clock (`std::chrono::steady_clock`) immediately before the measured `replay_phase` call | Immediately after the trial's drain-wait loop completes | Used only to compute throughput, not stored per-sample |
| Sustained throughput | `measured_order_count / batch_completion_seconds` | — | Reported in `orders/second` and `M orders/second`. **Caveat**: `replay_phase` paces pushes at `DEFAULT_ARRIVAL_RATE_HZ` (100 kHz) via open-loop pacing — this number reflects the *paced arrival rate the harness was told to replay*, not the engine's maximum sustained capacity. A run with 0 dropped events proves the engine kept up with 100 kHz; it does not by itself prove a higher rate is sustainable. |
| Saturation throughput | — | — | **Not implemented this pass** — see §6 (Known Limitations). A dedicated closed-loop, unpaced `--saturation` mode would be needed to measure this; do not infer a saturation-throughput claim from the paced number above. |
| Backpressure/drop behavior | — | — | `dropped_events`, `max_schedule_drift_ns` (existing); a queue-occupancy high-water-mark was scoped but **not implemented this pass**. |

### 2a. Why queue push latency is an aggregate, not a per-sample field

The successful `push()` call's own duration isn't known until *after* the
`Order` value has already been copied into the queue — there is no way to
retroactively embed "how long my own enqueue took" into the value that
enqueue just finished copying. Rather than approximate this, it's measured
honestly as a trial-level mean/max over every successful push in the
measured phase (`ReplayPhaseResult::push_ns_sum`/`push_ns_max` in
`benchmark.cpp`), surfaced in `--verbose` output. `queue_residence_ns` is
therefore a **disclosed approximation**: it includes the producer's own
push-call duration folded in (since the only producer-side timestamp
available to the consumer is `order.timestamp_ns`, stamped *before* the push
attempt, not after). At a validity-gated 0-dropped-events run this folded-in
duration is on the order of the measured push-latency aggregate (tens of
nanoseconds), not a material distortion — but it is not a perfectly clean
boundary, and this document says so rather than silently rounding the
seam away.

## 3. Clock

`rdtscp`, serialized with `lfence` on both sides (`clock.hpp`), calibrated
once per run (`calibrate_ns_per_cycle()`) and shared across threads via
`PipelineContext::ns_per_cycle` (thread-launch is the synchronization point
— see the code comment there for why an explicit fence isn't needed). Every
report prints **Clock overhead: X ns/sample** — the mean of 10,000
back-to-back `rdtsc_now()` calls, so the instrumentation's own fixed cost is
disclosed, not hidden inside the numbers it's measuring.

## 4. Percentile method

`hydra::stats::percentile()` — nearest-rank, `idx = floor(p/100 * N)`,
clamped to `N-1`. Deterministic, no interpolation between adjacent ranks.
P50/P90/P99/P99.9/max/mean are all computed from the same sorted sample
vector per trial (`hydra::stats::compute_percentiles`).

**Median across trials**: `hydra::stats::median_across_trials()` — true
median (average of the two middle elements for an even trial count, the
single middle element for odd). At the canonical `DEFAULT_TRIAL_COUNT = 5`
this is an exact median, not an approximation.

**Order population**: every non-cancel sample (new orders AND replaces,
since both go through the matching engine, just via different entry
points — `Matcher::match()` vs. `Matcher::replace()`). Cancels are tracked
as an entirely separate population (`cancel_pct` in `TrialStats`), never
folded into the order figures, never silently dropped either.

## 5. Statistics

`include/hydra/stats.hpp` — `mean`, `stddev` (sample, n-1 denominator, 0 for
n<2), `mad` (median absolute deviation), `coefficient_of_variation`,
`confidence_interval_95` (normal approximation, only reported as defensible
at n≥5 trials — an approximation disclosed as such, not a rigorous
small-sample interval). "Stability verdict" (STABLE/UNSTABLE in the console
report) is a heuristic threshold — CV < 15% on both P99 and P99.9 across
trials — not a formal statistical test.

## 6. Fail-closed validity

A run is marked `INVALID` (both in the console report's "Validity" line and
the CSV's `valid`/`invalid_reason` columns) when any of:

- Dirty working tree at build time, **unless** `--allow-dirty` was passed
  (in which case it's allowed but prominently reported as dirty in every
  artifact, never silently treated as clean).
- Any dropped event (queue backpressure) in any trial.
- Any arena fallback (heap allocation past the fixed PMR arena) in any
  trial.
- Any object-pool exhaustion (order or level pool) in any trial.
- The determinism check fails (see §7).

Preflight (core isolation, `performance` governor, SMT-off) failing throws
*before* any measurement work happens at all — this was already correct in
the prior harness and is unchanged.

**Not implemented this pass**: a reference-implementation cross-check (an
independent, deliberately-simple re-implementation of matching to diff
fills against). `reference_validation_status` is honestly reported as
`NOT_RUN` in every artifact — never fabricated as `PASS`. Building a real
reference matcher for both Price-Time and Pro-Rata is substantial,
separate engineering work; see §8.

## 7. Determinism check

Every trial replays the *identical* seeded dataset against a freshly reset
book (`OrderBook::reset()` + pool `reset()`s between trials). A
deterministic system must therefore produce identical **logical outcomes**
every time. `compute_trial_stats()` folds (FNV-1a-style) the following
counters into a `state_hash` per trial: final bid/ask level counts,
crossing/non-crossing/partial-fill/multi-level-sweep counts, self-trade
skips, resting/eligible orders examined, generated fill count, max fan-out,
order/cancel sample counts. **Deliberately excludes every timing field** —
latency legitimately varies run to run even in a perfectly deterministic
system, so including it would make the check meaningless.
`determinism_status = PASS` iff every trial's `state_hash` is identical to
trial 0's. A `FAIL` here is one of the `INVALID` triggers above.

## 8. Known limitations / deferred scope (read before citing anything)

Disclosed here rather than silently omitted, per this project's own
standard:

1. **Saturation throughput** (closed-loop, unpaced max-rate measurement) —
   not implemented. The reported throughput is the paced-rate figure (§2).
2. **Queue-occupancy high-water-mark** — not implemented.
3. **Reference-implementation cross-check** — not implemented;
   `reference_validation_status` is always `NOT_RUN`.
4. **`remainder_units_distributed` (Pro-Rata)** — not separately
   instrumented; always reported as `N/A`. Would require summing
   per-fill residual/extra allocations inside `pro_rata_at_level`.
5. **Workload-classification dataset library** (the ~26 hand-constructed
   scenarios: fixed eligible-order counts, tied-largest cases, FOK
   success/rejection, self-trade-heavy, deep-book, etc.) — **not built
   this pass**. `DatasetGenerator` gained the knobs needed to approximate
   several of these via ratios (`replace_ratio`, `client_id_count` for
   self-trade density), but a dedicated deterministic constructive
   generator (`workload_dataset_builder.hpp` in the original plan) with
   ground-truth sidecar metadata was scoped and deferred as
   `DEFERRED — NONCRITICAL`. Every number in this document's canonical
   runs is measured against the single mixed `datasets/bench_110k.bin`
   workload, same as before this redesign — the redesign's contribution is
   *separating and correctly labeling* Price-Time vs. Pro-Rata, the
   measurement categories, and the statistics, not yet *diversifying* the
   workload.
6. **`--saturation` mode, `blitz_ablation_report` compiled tool,
   `gen_workload_datasets` tool** — scoped in the plan, not built. The
   ablation suite's verdict classification instead runs as an embedded
   Python block in `scripts/run_ablations.sh`, a direct (not
   independently-implemented-twice) port of `hydra::stats::classify_ablation`.
7. **The ablation verdict noise-floor heuristic has an observed
   asymmetry**: `classify_ablation()`'s noise floor is
   `max(baseline_cv, ablated_cv) * baseline_p99`. When the *ablated* side is
   itself highly unstable trial-to-trial (observed live for the `unpinned`
   Price-Time run — OS-scheduled unpinned threads have large scheduling
   jitter), that instability inflates the noise floor enough to produce an
   `INCONCLUSIVE` verdict even when the raw delta is enormous (~39x in the
   run that exposed this). Read the raw median-P99.9 numbers in
   `ablation_summary.md`/`.csv` alongside the verdict, not the verdict
   alone, until this heuristic is refined (e.g. a proper two-sample test
   instead of a CV-based floor).

## 9. Canonical commands

```bash
# Price-Time
numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob --benchmark --mode price_time --trials 5 \
    --dataset datasets/bench_110k.bin --output results/<commit>/price_time.csv

# Pro-Rata
numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob --benchmark --mode pro_rata --trials 5 \
    --dataset datasets/bench_110k.bin --output results/<commit>/pro_rata.csv

# Ablation suite (both policies, all 4 variants -- 10 runs)
./scripts/run_ablations.sh --trials 5
```

Each canonical run writes three artifacts from one `--output PATH`:
`PATH` (summary CSV, one row per trial), `PATH` with `.raw` inserted before
the extension (one row per sample), and `<stem>.meta.json` (environment +
provenance).
