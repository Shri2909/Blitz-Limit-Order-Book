# metrics/ — visual proofs, generated from the real project

One script, one report. Everything in this folder exists to answer "prove
it" for the claims this codebase makes about itself, using real measured
numbers from actually running the code — not hand-drawn diagrams or
illustrative estimates (except where explicitly labeled as such).

## Run it

```
./metrics/generate_metrics.sh          # full run (~10-15s of actual measurement time)
./metrics/generate_metrics.sh --quick  # shortened threaded-pipeline windows, for a fast smoke test
```

Then open `metrics/out/report.html` in a browser. Re-running always
overwrites `metrics/out/` fresh — nothing here is meant to be hand-edited or
committed (see `.gitignore`).

Requires: the same toolchain the main project needs (CMake, a C++20
compiler), plus `python3` (stdlib only — no pip install) for the report
renderer. `perf` is optional (Category D2 reports "not available" cleanly
without it).

## What gets measured, and why it's organized this way

**A — Proofs from already-verified behavior.** Things the project's test
suite already established as correct; re-measured fresh on every run so the
report never goes stale relative to the code.
- A1: `cancel_order()` is O(1) — head vs. tail cancel latency swept across
  FIFO depth, both flat.
- A2: price-time vs. pro-rata produce different, individually-correct fills
  on the identical starting book.
- A3: SPSC queue occupancy over time under an unthrottled synthetic
  producer — shows exactly where backpressure kicks in.
- A4: every phase's test binary, run fresh, with pass/fail counts and which
  ones run under ASan/TSan (read directly from CMakeLists.txt, not
  hardcoded).
- A5: `ObjectPool` exhaustion is counted and instantly recoverable, never a
  crash.

**B — Quick measurements, on whatever machine ran the script.** Real
pipeline behavior (latency distribution, throughput), captured honestly —
**not** run through `run_benchmark()`'s preflight gate, so these numbers
carry a caveat banner in the report and should never be quoted as the
project's actual latency target. Useful for confirming the pipeline works
end-to-end and for spotting gross regressions, not for a performance claim.

**C — Structure, no measurement.** The Order/Level/FillEvent cache-line
layout, drawn from `sizeof`/`alignof`/`offsetof` on the live struct
definitions (via `struct_layout.cpp`) so the diagram can't silently drift
out of sync with `types.hpp`.

**D — The real, preflight-gated benchmark.** Actually invokes
`blitz_lob --benchmark` and, if that succeeds, wraps a trial in
`perf stat` for hardware counters. Both are allowed to report "not
available on this machine" without failing the script — an untuned host
refusing to produce a benchmark number is `run_preflight_checks()` (see
`src/benchmark.cpp`) working as designed, not a bug in this tooling. To get
real Category D numbers, run this script on a machine configured per
`config.hpp`'s `ENVIRONMENT_REQUIREMENTS` block (`isolcpus=`, performance
governor, SMT off).

## Layout

```
metrics/
  generate_metrics.sh   orchestrator: builds the project, runs every
                         measurement, invokes the report renderer
  generate_report.py    reads metrics/out/*.csv, writes report.html
                         (stdlib only, hand-rolled inline SVG charts)
  src/                  standalone C++ measurement programs (A1-A5, B1-B2, C1)
                         -- deliberately NOT wired into the main
                         CMakeLists.txt; compiled directly by the shell
                         script against include/ so this folder never
                         needs to touch the main build
  out/                  generated CSVs + report.html (gitignored, always
                         safe to delete and regenerate)
```

## Design notes

- The six `src/*.cpp` programs call the real project headers
  (`hydra/matcher.hpp`, `hydra/order_book.hpp`, `hydra/pipeline.hpp`, ...)
  directly — they orchestrate real API calls and print structured CSV, they
  never reimplement any matching/book/pool logic themselves. A chart here
  is a chart of the actual code, not a simulation of it.
- Every category degrades gracefully. A build failure in one phase's test
  target doesn't block the others; `perf` being unavailable doesn't block
  A/B/C; an untuned host doesn't block anything except D's real numbers
  (which report why, specifically).
- Charts are hand-rolled inline SVG (see `generate_report.py`), using the
  validated categorical palette from the project's dataviz conventions —
  no charting library, no CDN, no network access needed to view the report.
