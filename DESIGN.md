# Design Decisions

For each hot-path choice: the problem, the alternative that was rejected, what
the ablation measured, and where it lives in the code. Structure and data flow
are in [`ARCHITECTURE.md`](ARCHITECTURE.md); timing and verdict definitions in
[`METHODOLOGY.md`](METHODOLOGY.md).

> **Measurement vintage.** All latency tables below were measured at commit
> `98a597b` on 2026-07-21 (clean tree, i5-1235U, cores 4/5), before later
> correctness fixes changed matching outcomes. Treat them as evidence about the
> design choices on the earlier engine, and re-measure before quoting them as
> current performance. Raw data: `results/98a597b/ablation_*.csv`,
> consolidated in [`CONSOLIDATED_BENCHMARK_RESULTS.md`](CONSOLIDATED_BENCHMARK_RESULTS.md).

**Reference baseline** (the ablation suite's own baseline run, replaying
`bench_110k.bin`, median of 5 trials): price-time P99 1,222 ns and P99.9
1,762 ns with a 137 ns P99.9 spread (max minus min); pro-rata P99 1,428 ns and
P99.9 3,065 ns. The separate canonical run quoted in the README (1,850 ns
price-time P99.9) is a different run of the same binary. All ratios below are
ablated divided by baseline.

## How the ablations work

Each ablation is a separate build target whose include path puts a modified
copy of exactly one header from `ablation/<name>/include/hydra/` ahead of the
real one. Production sources in `src/` and `include/` are never edited, and every
variant runs through the same gated `--benchmark` path.

| Target | Override | Change |
|---|---|---|
| `blitz_lob_ablation_mutex_queue` | `spsc_queue.hpp` | `std::mutex` + `std::queue<T>` behind the same API |
| `blitz_lob_ablation_unpinned` | `affinity.hpp` | `pin_to_core` / `verify_affinity` do nothing (host preflight still runs) |
| `blitz_lob_ablation_malloc_pool` | `object_pool.hpp` | `new`/`delete` per object instead of the slab |
| `blitz_lob_ablation_flat_layout` | `types.hpp`, `dataset_generator.hpp` | Flat 88-byte `Order` without the hot/cold split |

`DatasetRecord` embeds `Order`, so the flat-layout build changes the dataset's
on-disk record size and needs its own file (`ablation4_flat.bin`, same seed and
flags, made by `blitz_gen_dataset_ablation_flat_layout`). The logical event
sequence is identical to `bench_110k.bin`.

## Lock-free SPSC queue

**Problem.** One `Order` crosses the producer-to-matcher boundary per event, so
whatever does the handoff is on the hot path every time. A lock lets the
scheduler park the matching thread, which undoes the isolation work below.

**Rejected.** A `std::mutex`-guarded `std::queue<Order>`.

| Price-time | P99 | P99.9 | P99.9 spread |
|---|---|---|---|
| Lock-free SPSC (baseline) | 1,222 ns | 1,762 ns | 137 ns |
| Mutex + queue | 5,484 ns | 7,770 ns | 915 ns |
| Ratio | 4.49x | 4.41x | 6.68x |

**Verdict: PROVEN** on both policies (pro-rata P99.9 3,065 to 7,774 ns, 2.54x).

**Implementation** (`spsc_queue.hpp`). `buffer_`, `tail_`, and `head_` are each
`alignas(64)` so producer and consumer indices never share a line. `push` and
`pop` do one relaxed load of the caller's own index, one acquire load of the
other's, and one release store: no CAS, no lock. Capacity is a compile-time
power of two so indexing is a bit mask. `T` must be trivially copyable so the
by-value handoff cannot tear.

## Core pinning and CPU isolation

**Problem.** An OS-scheduled thread can be preempted or migrated at any time.
Migration means cold caches and TLB; preemption adds an unbounded delay. Either
shows up as unexplained tail latency.

**Rejected.** Unpinned, OS-scheduled threads.

| Price-time | P99 | P99.9 | P99.9 spread |
|---|---|---|---|
| Pinned (baseline) | 1,222 ns | 1,762 ns | 137 ns |
| Unpinned | 1,098 ns | 9,216 ns | 139,917 ns |
| Ratio | 0.90x | 5.23x | 1,021x |

**Verdict: INCONCLUSIVE under price-time, PROVEN under pro-rata** (P99.9 3,065
to 4,906 ns, 1.60x). Under price-time the unpinned median P99 is lower than
the pinned one, and the verdict rule requires the P99 delta to clear a noise
floor, so it cannot call the effect. The tail itself is the evidence: the five
unpinned trials' P99.9 range from 4,729 to 144,646 ns on one binary, against a
1,698 to 1,835 ns band when pinned. A run with more trials or a longer window
would be needed to turn the price-time verdict into a clean call.

**Implementation** (`affinity.hpp`, `src/pipeline.cpp`, `src/benchmark.cpp`).
`pin_to_core` sets a single-core mask with `pthread_setaffinity_np`;
`verify_affinity` reads the mask back and throws unless it is exactly one core,
so a silently ignored request is fatal. Both thread bodies call the pair before
their loops. `run_preflight_checks()` independently verifies that both cores are
isolated, on the `performance` governor, and without an SMT sibling. The
development host's cores 4/5 are E-cores with no SMT sibling; cores 2/3 are
siblings of one P-core, so choose and verify cores on any other machine.

## Fixed-slab object pools

**Problem.** A `new`/`malloc` per order brings allocator locks or per-thread
arena bookkeeping, and arrival-driven alloc/free patterns can fragment the
heap in a way that is hard to bound.

**Rejected.** Per-order `new`/`delete` for `Order` objects.

| Price-time | P99 | P99.9 | P99.9 spread |
|---|---|---|---|
| Object pool (baseline) | 1,222 ns | 1,762 ns | 137 ns |
| `new`/`delete` | 2,167 ns | 6,079 ns | 850 ns |
| Ratio | 1.77x | 3.45x | 6.20x |

**Verdict: PROVEN** on both policies (pro-rata P99.9 3,065 to 6,948 ns, 2.27x).

**Implementation** (`object_pool.hpp`, `order_book.hpp`). `ObjectPool<T, N>`
holds one inline `alignas(64)` slab (heap-allocated once at startup, never a
stack local) and threads an intrusive free list through a `union Slot`.
`acquire()` and `release()` are plain pointer swaps with no atomics, which is
correct only because each pool has a single owning thread; relaxing that would
require a tagged-pointer CAS list. Exhaustion returns `nullptr`.

The book extends the same goal to its lookup structures: `order_index_` is a
`std::pmr::unordered_map` and the bid/ask maps are `std::pmr::map`, all backed
by fixed `monotonic_buffer_resource` arenas with a counted heap fallback. Cancel
is an O(1) hash lookup to `Order*` and `Level*`, then an O(1) unlink from the
intrusive doubly linked FIFO. `test_cancel_is_position_independent_micro_benchmark`
and `test_cancel_latency_is_depth_independent` in
`tests/test_phase5_orderbook.cpp` check that cancel cost stays flat with FIFO
position and depth.

## Hot/cold cache-line split

**Problem.** Matching reads price, quantity, policy fields, and FIFO links on
every visit. Keeping them in one cache line shrinks the footprint of the common
case. Timestamps and client tags are metadata (although `client_id` is read for
self-trade checks).

**Rejected.** A flat `Order` with the same fields and no split, padding, or
`alignas(64)`: 88 bytes instead of 128.

| Price-time | P99 | P99.9 | P99.9 spread |
|---|---|---|---|
| Hot/cold split, 128 B (baseline) | 1,222 ns | 1,762 ns | 137 ns |
| Flat, 88 B | 1,231 ns | 1,797 ns | 77 ns |
| Ratio | 1.01x | 1.02x | 0.56x |

**Verdict: INCONCLUSIVE** on both policies (pro-rata P99.9 3,065 to 3,554 ns,
1.16x). At 1.02x the difference is smaller than the ablation's own trial spread,
so there is no measurable signal here. Two caveats apply. The flat struct is
also smaller, so the comparison bundles "no split" with "smaller struct"; an
ablation that keeps 128 bytes and only un-separates the fields would isolate the
layout. And the metric is end-to-end latency, not IPC: IPC was not measured
(`perf stat` was blocked by `perf_event_paranoid`), so no IPC improvement is
claimed.

**Implementation** (`types.hpp`). `Order` is `alignas(64)` and exactly 128
bytes. Line 0: `order_id`, `price`, `qty`, `side`, `tif`, `event_tag`, 25 bytes
of padding, `prev_`, `next_`. Line 1: `timestamp_ns`, `client_id`,
`client_tag[32]`, 16 bytes of padding. `Level` is 64 bytes. Sizes, alignment, and
trivial copyability are `static_assert`ed.

## Not ablated

Pro-rata and price-time are both implemented and benchmarked, but there is no
ablation for the O(1) intrusive-list cancel, the PMR arenas, or the serialized
`RDTSCP` clock; they are justified by design and covered by tests only.

## Reproducing the ablations

On a tuned host (see [`ALL_COMMANDS.md`](ALL_COMMANDS.md)), from a clean tree:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target blitz_lob blitz_gen_dataset -j"$(nproc)"
./build/blitz_gen_dataset --seed 42 --count 110000 --output datasets/bench_110k.bin
./scripts/run_ablations.sh --trials 5
```

The script builds the baseline and the four variants, regenerates the
flat-layout dataset, runs both matching policies (10 runs), and writes
`results/<hash>/ablation_summary.{csv,md}` plus each run's CSV, `.raw.csv`, and
`.meta.json`. Expect the same direction as the tables above (every variant
slower than baseline) but not the same multipliers on a different machine or
session. The dataset flags are recorded in `datasets/manifest.txt`.
