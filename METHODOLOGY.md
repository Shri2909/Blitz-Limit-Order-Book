# Measurement Methodology

Six diagrams answering one question: **how does a number in this project
earn the right to be quoted?** Every latency figure in `README.md` traces
back to a precise timestamp boundary, a specific statistical transform,
and two independent pass/fail gates — this document is that trace, made
visual. Nothing here is aspirational: every diagram is a direct
translation of `src/benchmark.cpp`, `include/hydra/stats.hpp`, and
`scripts/run_ablations.sh`'s actual behavior.

**Scope note:** this is the *measurement* layer specifically — for what
the system is built of, see [`ARCHITECTURE.md`](ARCHITECTURE.md); for why
each design decision was made, see [`DESIGN.md`](DESIGN.md); for the
numbers themselves, see [`README.md`](README.md). The governing rule,
carried over unchanged from this project's own methodology notes: **if a
claim made anywhere else — README, a resume, a conversation — can't be
traced to a definition in this document, don't make the claim.**

---

## Contents

0. [The measurement pipeline, end to end](#0-the-measurement-pipeline-end-to-end)
1. [Measurement boundary timeline](#1-measurement-boundary-timeline)
2. [Statistical pipeline](#2-statistical-pipeline)
3. [Fail-closed validity gate](#3-fail-closed-validity-gate)
4. [Determinism check](#4-determinism-check)
5. [Ablation verdict classification](#5-ablation-verdict-classification)

---

## 0. The measurement pipeline, end to end

```mermaid
flowchart LR
    DS[("Seeded dataset\nbench_110k.bin, seed=42")] --> REPLAY["replay_phase()\nopen-loop pacing @ 100kHz\n-- the REAL production path,\nnot a mock matcher"]
    REPLAY --> SAMPLES[("Per-sample latencies\nsee §1")]
    SAMPLES --> STATS["Statistics\nsee §2"]
    SAMPLES --> HASH["Determinism check\nsee §4"]
    STATS --> GATE
    HASH --> GATE{"Fail-closed validity gate\nsee §3"}
    GATE -->|"any trigger fires"| INVALID["INVALID\nreported, not discarded"]
    GATE -->|"nothing triggered"| VALID["VALID"]
    VALID --> CSV[("results/&lt;hash&gt;/*.csv\n+ .meta.json")]

    classDef stage fill:#2a78d6,color:#fff,stroke:#1c5cab
    classDef gate fill:#7a8ba0,color:#fff,stroke:#52514e
    classDef bad fill:#fff8e6,color:#8a6200,stroke:#fab219
    classDef good fill:#e8f5e9,color:#0ca30c,stroke:#0ca30c
    class DS,REPLAY,SAMPLES,STATS,HASH stage
    class GATE gate
    class INVALID bad
    class VALID,CSV good
```

**What this shows:** the full path from a seeded dataset to a number
that's actually safe to cite, with each stage pointing at the diagram
below that explains it in detail. **What it proves:** `replay_phase()`
drives `matching_thread_fn` — the identical production code path the live
pipeline uses, popping from the real `SpscQueue`, calling the real
`Matcher::match()`/`replace()`, mutating the real `OrderBook`. Benchmark
mode is not a simplified stand-in for production matching; it's
production matching with a paced producer and a recorder attached. Every
sample that comes out of that replay has to survive *two independent*
checks — a resource-safety gate (§3) and a logical-determinism gate (§4)
— before it's allowed to become a quoted figure.

---

## 1. Measurement boundary timeline

```mermaid
flowchart LR
    T0(("order.timestamp_ns\nintended-arrival stamp,\ntaken before push"))
    T0 -->|"queue_residence_ns\nincludes the producer's own\npush-call duration --\ndisclosed as folded-in, not hidden"| T1
    T1(("t_pop_start\nimmediately before\na successful pop()"))
    T1 -->|"queue_pop_ns"| T2(("t_pop_end\npop() returns"))
    T2 -.->|"dequeued, about to match"| T3(("t_match_start\nimmediately before\nmatch() / replace()"))
    T3 -->|"match_time_ns\nmatching engine's own cost,\nMINUS fill_publish_ns"| T4(("t_match_end"))
    T3 -.->|"fill_publish_ns\non_fill callback, summed\nacross every fill generated"| T4
    T0 ==>|"end_to_end_ns -- what every\n'ns/order' figure in this\nproject actually means"| T4

    classDef marker fill:#0b0b0b,color:#fff,stroke:#000
    class T0,T1,T2,T3,T4 marker
```

**What this shows:** the five named timestamps the harness actually takes
per order, and which of the seven CSV latency columns spans which gap
between them — the dense boundary table in the methodology notes, turned
into something you can trace with your eyes instead of cross-referencing
row by row.

**What it proves:** `end_to_end_ns` (the figure behind every headline
number in this project) is not a single measurement — it's the sum of
every interval below it, including one deliberately disclosed
imperfection: `queue_residence_ns` folds in the producer's own push-call
duration, because the consumer's only available producer-side timestamp
(`order.timestamp_ns`) is stamped *before* the push attempt, not after.
There's no way to retroactively embed "how long my own enqueue took" into
a value that enqueue just finished copying — so rather than approximate
it silently, that seam is measured separately (a trial-level mean/max,
surfaced in `--verbose` output) and named explicitly here instead of
smoothed over. `match_time_ns` is also drawn with a visible fork: it
explicitly *excludes* `fill_publish_ns`, which is tracked as its own
column precisely so a crossing order's fill-callback cost is never
silently absorbed into "the matching engine's own cost."

---

## 2. Statistical pipeline

```mermaid
flowchart LR
    RAW[("Raw per-sample latencies\none trial, N samples")] --> PCT["percentile()\nnearest-rank: idx = floor(p/100*N),\nclamped to N-1 -- no interpolation"]
    PCT --> TP["Per-trial P50 / P90 / P99 /\nP99.9 / max / mean"]
    TP --> REPEAT{"Repeated across\n5 trials"}
    REPEAT --> MED["median_across_trials()\ntrue median -- exact at\n5 trials, not approximated"]
    MED --> HEADLINE[("The quoted figure\ne.g. median P99.9 = 1850.0 ns")]
    TP --> CV["coefficient_of_variation\nacross the 5 trials' P99 & P99.9"]
    CV --> VERDICT{"CV < 15%\non both?"}
    VERDICT -->|yes| STABLE["STABLE"]
    VERDICT -->|no| UNSTABLE["UNSTABLE\nheuristic threshold,\nnot a formal statistical test"]

    classDef stage fill:#2a78d6,color:#fff,stroke:#1c5cab
    classDef verdict fill:#7a8ba0,color:#fff,stroke:#52514e
    class RAW,PCT,TP,MED,HEADLINE stage
    class CV,VERDICT,STABLE,UNSTABLE verdict
```

**What this shows:** the exact transform chain from raw per-sample
nanosecond readings to the single number this project ever quotes —
nearest-rank percentile per trial, then a true median across 5 trials,
with a separate stability check riding alongside it.

**What it proves:** "median P99.9" is doing real statistical work, not
standing in for "a P99.9 from one lucky trial." Nearest-rank percentile
computation is deterministic and reproducible — no interpolation choice
to second-guess — and at exactly 5 trials, `median_across_trials()`
computes a true, exact median (the single middle value), not an
approximation that only gets better with more samples. The
`coefficient_of_variation` branch is what produces the STABLE/UNSTABLE
read printed alongside every result — and it's explicitly labeled a
heuristic (a 15% CV threshold), not dressed up as a rigorous statistical
test it isn't.

---

## 3. Fail-closed validity gate

```mermaid
flowchart TD
    START(["Preflight: isolcpus present?\nperformance governor? SMT off?"]) -->|"any check fails"| ABORT["Refuse to run --\nzero measurement work happens"]
    START -->|"all pass"| RUN["Run 5 trials against\nthe replayed dataset"]
    RUN --> CHECKS{"Any INVALID\ntrigger fires?"}

    CHECKS -->|"dirty working tree\n(no --allow-dirty)"| INVALID
    CHECKS -->|"any dropped event\nin any trial"| INVALID
    CHECKS -->|"any arena fallback\nin any trial"| INVALID
    CHECKS -->|"any pool exhaustion,\norder or level"| INVALID
    CHECKS -->|"determinism check FAILs\n-- see §4"| INVALID
    CHECKS -->|"none of the above"| VALID

    INVALID["INVALID\nconsole + CSV's valid /\ninvalid_reason columns --\nnever silently kept"]
    VALID["VALID\nsafe to quote"]

    classDef bad fill:#fff8e6,color:#8a6200,stroke:#fab219
    classDef good fill:#e8f5e9,color:#0ca30c,stroke:#0ca30c
    class ABORT,INVALID bad
    class VALID good
```

**What this shows:** two layers of refusal, not one. The preflight gate
stops an untuned host *before any measurement happens at all*; a second,
independent set of five checks can still invalidate a run *after* it
completes, even on a perfectly-tuned host.

**What it proves:** this is the mechanism behind the `0` in this
project's hero-stats band — "0 dropped events across 70 measured trials"
isn't a claim that dropping is impossible, it's a claim that *if* any of
70 trials had dropped an event, exhausted a pool, fallen back to the
heap, or diverged from trial 0's logical outcome, that specific run would
carry `INVALID` in its own CSV rather than being quietly folded into a
median. `--allow-dirty` is the one deliberate escape hatch, and it's
designed to be loud, not silent: a dirty-tree run is still permitted, but
every artifact it produces reports the dirty flag prominently rather than
pretending the tree was clean.

---

## 4. Determinism check

```mermaid
flowchart LR
    SEED[("Same seeded dataset\nbench_110k.bin, seed=42")] --> TRIALS["5 trials, each against a\nfreshly reset OrderBook + pools"]
    TRIALS --> FOLD["compute_trial_stats()\nFNV-1a-style fold of: level counts,\ncrossing / non-crossing, partial fills,\nmulti-level sweeps, self-trade skips,\nfill count, max fan-out, order/cancel counts"]
    FOLD --> HASH[("state_hash\nper trial")]

    EXCLUDE["Deliberately excluded:\nevery timing field -- latency\nlegitimately varies run to run\neven in a deterministic system"]
    EXCLUDE -.->|"never folded in"| FOLD

    HASH --> CMP{"identical to\ntrial 0's hash?"}
    CMP -->|"yes, all 5"| PASS["determinism_status = PASS"]
    CMP -->|"any trial differs"| FAIL["determinism_status = FAIL\none of §3's INVALID triggers"]

    classDef good fill:#e8f5e9,color:#0ca30c,stroke:#0ca30c
    classDef bad fill:#fff8e6,color:#8a6200,stroke:#fab219
    class PASS good
    class FAIL bad
```

**What this shows:** what actually goes into the `state_hash` this
project prints alongside every result, and — just as importantly — what's
deliberately left out of it.

**What it proves:** determinism here means "identical *logical* outcome,"
not "identical timing." A system whose fill counts, sweep depths, and
self-trade-skip counts are bit-for-bit identical across 5 independent
trials of the same seeded dataset, while its *latencies* naturally vary
run to run, is exactly the behavior a correct deterministic matching
engine should show — and folding timing into the hash would make the
check meaningless, flagging normal scheduling jitter as a correctness
failure. This is the concrete mechanism behind every "state_hash identical
across all trials" line in `CONSOLIDATED_BENCHMARK_RESULTS.md`.

---

## 5. Ablation verdict classification

```mermaid
flowchart TD
    BASELINE[("Baseline: 5 trials\nP99.9 values + CV")] --> DELTA
    ABLATED[("Ablated variant: 5 trials\nP99.9 values + CV")] --> DELTA

    DELTA["raw_delta =\nablated_median_P99.9 /\nbaseline_median_P99.9"]

    BASELINE --> FLOOR
    ABLATED --> FLOOR
    FLOOR["noise_floor =\nmax(baseline_cv, ablated_cv)\n* baseline_P99.9"]

    DELTA --> COMPARE{"does raw_delta clear\nthe noise floor?"}
    FLOOR --> COMPARE

    COMPARE -->|"yes, comfortably"| PROVEN["PROVEN"]
    COMPARE -->|"no -- within\nnoise-floor range"| INCONCLUSIVE["INCONCLUSIVE"]

    INCONCLUSIVE -.-> NOTE["Disclosed asymmetry: if the ABLATED side\nis itself unstable (e.g. unpinned threads'\nscheduling jitter), that inflates the floor\nenough to read INCONCLUSIVE even at a ~39x\nraw delta, observed live for one unpinned run.\nRead the raw median-P99.9 numbers alongside\nthe verdict, not the verdict alone."]

    classDef calc fill:#2a78d6,color:#fff,stroke:#1c5cab
    classDef good fill:#e8f5e9,color:#0ca30c,stroke:#0ca30c
    classDef warn fill:#fff8e6,color:#8a6200,stroke:#fab219
    class DELTA,FLOOR calc
    class PROVEN good
    class INCONCLUSIVE,NOTE warn
```

**What this shows:** the exact arithmetic behind every PROVEN/INCONCLUSIVE
label in `README.md`'s and `ARCHITECTURE.md`'s ablation tables — not a
judgment call made while writing up results, but a formula applied
uniformly to every design decision tested.

**What it proves:** the noise floor is deliberately keyed to *whichever
side is noisier*, baseline or ablated — a reasonable design, but one with
an honestly-documented sharp edge: when the ablated variant is inherently
unstable (unpinned OS-scheduled threads being the textbook case), its own
instability inflates the floor enough to swallow even an enormous raw
delta. That's precisely the mechanism behind core pinning's INCONCLUSIVE
verdict under price-time matching elsewhere in this project — not a
failure of the ablation methodology, but a documented consequence of a
CV-based floor meeting a variant whose whole defect *is* variance. The
project's own stated fix for this — a proper two-sample statistical test
in place of a CV-based floor — is listed as future work, not silently
left unmentioned.

---

## Further reading

- **[`README.md`](README.md)** — the numbers this methodology produces,
  and the reproduction commands that regenerate them.
- **[`ARCHITECTURE.md`](ARCHITECTURE.md)** — the system these
  measurements are taken against: component structure, concurrency model,
  and the AF_XDP path referenced in §0 above.
- **[`DESIGN.md`](DESIGN.md)** — why each measured design decision was
  made, and the full ablation table §5's classification algorithm feeds.
- **`scripts/run_ablations.sh`** — the actual implementation of §5's
  `classify_ablation()` logic, ported directly from
  `hydra::stats::classify_ablation` rather than re-implemented by hand.

All source references above were verified against this project's own
methodology notes and the current working tree; if a referenced function
or file has moved, trust the code over this document and open an issue if
the drift is more than cosmetic.
