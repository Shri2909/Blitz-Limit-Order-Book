# Design Decisions

For each design choice: what it is, why, what was measured, what it showed, and the honest conclusion. Structure and data flow are in [`ARCHITECTURE.md`](ARCHITECTURE.md); timing, statistics and the verdict rule are in [`METHODOLOGY.md`](METHODOLOGY.md).

## Where the evidence comes from

- **Final ablation suite** (stored locally in `results_new/77eb286/`, gitignored): commit `77eb286`, 2026-11-12, i5-1235U with cores 4/5 on an isolated, fully tuned E-core cluster (`sudo scripts/tune_host.sh 4,5,6,7`), 5 trials per arm, both matching policies. Each verdict compares per-trial P99.9 of a variant against the baseline of the same session ([rule](METHODOLOGY.md#ablation-verdicts)).
- **Development A/B runs** (allocator and ring-slot changes): interleaved rounds on the pinned cores, 8 rounds per mode, paired differences, faster in 8/8 rounds with an identical state hash. **These logs are not in the repo**; the numbers below are quoted from the development session and cannot be re-checked from a checkout.
- **One-off experiments** (the pinning factorial at commit `9c5813f`, a cache-simulation analysis of the layout): also not reproducible from repo scripts, and labelled each time.
- The ablation baseline is a separate run of the same binary from the canonical run quoted in the [README](README.md) (P99.9 1,148 vs 1,158 ns price-time, 2,343 vs 2,433 pro-rata). Both are labelled wherever they appear. Everything is in-process, synthetic-data, single-host, single-session latency.

| Choice | Alternative removed | Price-time P99.9 (baseline 1,148) | Pro-rata P99.9 (baseline 2,343) | Verdict |
|---|---|---|---|---|
| Lock-free SPSC ring | `std::mutex` + `std::queue` (same payload) | 2,246 ns, 1.96x | 3,363 ns, 1.44x | PROVEN, PROVEN |
| Fixed-slab pools | `new`/`delete` per object | 4,165 ns, 3.63x | 4,330 ns, 1.85x | PROVEN, PROVEN |
| Hot/cold cache-line split | Flat 88-byte `Order` (same queue) | 1,120 ns, 0.98x | 2,268 ns, 0.97x | NO_MEASURABLE_EFFECT, NO_MEASURABLE_EFFECT |
| Core isolation + pinning | Unpinned threads | 2,435 ns, 2.12x | 1,782 ns, 0.76x | PROVEN (by a 4 ns margin), ABLATED_FASTER (see below: this row does not measure pinning) |

## How the ablations work

Each ablation is a separate build target whose include path puts a modified copy of exactly one header from `ablation/<name>/include/hydra/` ahead of the real one. Production sources in `src/` and `include/` are never edited, and every variant runs through the same gated `--benchmark` path.

| Target | Override | Change |
|---|---|---|
| `blitz_lob_ablation_mutex_queue` | `spsc_queue.hpp` | `std::mutex` + `std::queue` (a deque) of the same eight-field payload the ring slot carries, behind the same API |
| `blitz_lob_ablation_unpinned` | `affinity.hpp` | `pin_to_core` / `verify_affinity` do nothing (host preflight still runs) |
| `blitz_lob_ablation_malloc_pool` | `object_pool.hpp` | `new`/`delete` per object instead of the slab |
| `blitz_lob_ablation_flat_layout` | `types.hpp`, `dataset_generator.hpp` | Flat 88-byte `Order` without the hot/cold split; still served by the same one-slot queue |

`DatasetRecord` embeds `Order`, so the flat-layout build changes the on-disk record size and replays its own file (`ablation4_flat.bin`, same seed and flags). Arms are payload-matched (commit `77eb286`): the queue and the mutex arm move the same eight fields for every `Order` layout, so each ablation changes one thing. Per-arm tables below show the median and the five per-trial P99.9 values, in ns.

## Lock-free SPSC ring

**What and why.** One `Order` crosses the producer-to-matcher boundary per event, so the handoff is on the hot path every time. A lock can park the matching thread, which undoes the isolation work.

**Implementation** (`include/hydra/spsc_queue.hpp`). The production ring is `SpscQueue<Order, 65536>`: one 64-byte slot per event whose own sequence word is the "ready" flag and shares a cache line with every field the matcher reads, so the consumer polls the line that will carry the data (one cross-core transfer) instead of a tail index plus a two-line, 128-byte copy. The slot carries `order_id`, `price`, `qty`, `side`, `tif`, `event_tag`, `client_id` and `timestamp_ns`; the book's links, `client_tag` and padding are not transported and come back zeroed (`static_assert`s fail the build if `Order` gains a field the slot does not carry). The same slot serves the flat 88-byte layout of the layout ablation (an unknown `Order` layout fails the build). The ring shrank from 8 MiB to 4 MiB. The producer keeps a private copy of the consumer's last head, so it reads the consumer's line only when the ring looks full; the generic template (kept for other payload types, unused by the pipeline) caches both peers' indices. Indices and buffer are `alignas(64)`; there is no CAS and no lock; capacity is a power of two so indexing is a mask.

| Price-time P99.9 | Median | Trials |
|---|---|---|
| Lock-free ring (baseline) | 1,148 | 1,157 / 1,129 / 1,143 / 1,148 / 1,155 |
| Mutex + queue | 2,246 | 2,172 / 2,287 / 2,231 / 2,246 / 2,246 |

**Showed.** PROVEN under both policies: 1.96x price-time, 1.44x pro-rata (3,363 vs 2,343; trials 3,307-3,393 all above the baseline's 2,440 maximum). The clearest effect is the median: P50 is 861 vs 320 ns price-time (2.7x worse), 844 vs 326 pro-rata. The mutex arm's tail is host-scheduling driven (a blocked consumer sleeps and wakes), which is the point of the choice but is also why the ratio depends on the machine. **Payload matching.** The mutex arm originally copied the whole 128-byte `Order` while the ring moved one 64-byte slot, which inflated the ratio to 5.6x; commit `77eb286` made both move the same eight fields, and the 1.96x above is the lock-versus-lock-free difference with the container (a deque) still part of the arm. Lesson: check that an ablation changes only one thing before quoting its ratio.

**Development evidence (not stored in the repo).** Replacing the tail index plus 128-byte copy with the one-slot design: price-time P50 -61 ns, P99 -76, P99.9 -90; pro-rata -63 / -74 / -99. Caching the peer index, together with a lazy next-level lookup in sweeps and skipping a duplicate-id lookup: about -14% P50 and -7% P99 versus the previous commit.

**Conclusion.** The lock-free handoff is a clear, repeatable win on this host: smaller than the earlier unmatched comparison suggested, but PROVEN under both policies and visible at P50. The one-slot refinement is a measured but small development-session improvement.

## Fixed-slab object pools and the book's node allocator

**What and why.** A `new`/`malloc` per order brings allocator locks or arena bookkeeping, and arrival-driven alloc/free patterns can fragment the heap in ways that are hard to bound. `ObjectPool<T, N>` (`include/hydra/object_pool.hpp`) holds one inline `alignas(64)` slab (heap-allocated once at startup) and threads an intrusive free list through a `union Slot`. `acquire()` and `release()` are plain pointer swaps with no atomics, correct only because each pool has one owning thread. Exhaustion returns `nullptr` and is counted; it invalidates a run.

| Price-time P99.9 | Median | Trials |
|---|---|---|
| Slab pool (baseline) | 1,148 | 1,157 / 1,129 / 1,143 / 1,148 / 1,155 |
| `new`/`delete` | 4,165 | 4,237 / 4,094 / 4,165 / 4,203 / 4,043 |

**Showed.** PROVEN under both policies: 3.63x price-time, 1.85x pro-rata (4,330; trials 4,068-4,425 against baseline 2,305-2,440). P99 is 1,366 vs 851 ns price-time. **Scope.** The ablation replaces only the `Order` and `Level` pools. It says nothing about the book's lookup structures, which sit on separate allocators.

**The book's node allocator** (`order_book.hpp`). The order-ID hash map and the price-level maps are `std::pmr` containers over fixed arenas with a counted heap fallback. Their node allocator was `std::pmr::unsynchronized_pool_resource`; it is now `SizeClassFreeListResource`, one intrusive LIFO free list per 16-byte size class (up to 64 bytes; larger requests such as the hash bucket array go straight to the arena). A callgrind run measured roughly 150-175 instructions per allocate/deallocate for the pool resource against about 10 for the free list, and a freed block is reused immediately instead of the lowest free slot of a chunk. **Development evidence (not stored in the repo):** about -35% engine instructions; price-time P50 -32 ns, P99 -147, P99.9 -189; pro-rata -34 / -135 / -129. This one has no ablation target.

**Conclusion.** Pooling is a clear win and the allocator replacement was a worthwhile, repeatable step; the second claim rests on development A/B logs that are not in the repo.

## Hot/cold cache-line split (cache-line padding)

**What and why.** `Order` is `alignas(64)` and 128 bytes. Line 0 holds `order_id`, `price`, `qty`, `side`, `tif`, `event_tag`, padding and the FIFO links; line 1 holds `timestamp_ns`, `client_id`, `client_tag[32]` and padding (`include/hydra/types.hpp`, sizes `static_assert`ed). The intent: the fields a sweep needs sit together, and metadata stays out of the way, shrinking the working set of the common case.

| Price-time P99.9 | Median | Trials |
|---|---|---|
| Split, 128 B (baseline) | 1,148 | 1,157 / 1,129 / 1,143 / 1,148 / 1,155 |
| Flat, 88 B | 1,120 | 1,213 / 1,120 / 1,130 / 1,111 / 1,110 |

**Showed.** NO_MEASURABLE_EFFECT on both policies, at every percentile: price-time P50 320 vs 320 ns, P99.9 1,120 vs 1,148 (0.98x); pro-rata P99.9 2,268 vs 2,343 (0.97x; trials 2,261-2,304 against baseline 2,305-2,440). Both builds use the same one-slot queue, so the layout is the only change. It is still "not detected", not "equal", and the flat struct is also smaller, so size and layout are not separated. IPC was not measured (`perf_event_paranoid` blocked `perf stat`).

**Why it cannot show up on this workload** (analysis with valgrind/cachegrind on a single-thread replay of the canonical dataset, **not stored in the repo**; simulated, not hardware counters):
- Matching is only about 40% of a typical event (see the overhead breakdown below), so a layout gain is diluted.
- Sweeps read `client_id` for the self-trade check, and it lives on the cold line, so each examined resting order touches both lines: 2.00 order lines per examined order for the split layout, 1.70 flat, 1.0 for a one-line layout. The split does not reduce line count; if it helps at all it is as a footprint argument.
- Most fill makers are recent: 62% were last touched within 10 events and 90% within 100, so they are L1/L2 hits either way.
- The book grows from about 1,200 to 14,300 resting orders and never reaches steady state; the live set is 1.75 MiB of orders (2.75 MiB with the index) against a 2 MB L2 shared by cores 4-7, so the footprint pressure the split targets is barely engaged. Only about 3% of events are real cancels because the generator aims 80% of them at already-dead IDs.
- Simulated L2 misses per event: 0.76 split, 0.65 flat, 0.55 one-line layout.
- On a synthetic deep book (sweeps of 20 orders over 20,000 resting per side) on a P-core, flat was 12% faster than split and the one-line layout 17% faster.

**Conclusion.** The honest claim is "the split is a footprint argument, not a line-count argument, and this workload cannot show it". It is not an optimization proven here, and nothing in these runs argues for keeping it on performance grounds; it is kept as a documented layout, not a demonstrated win. A one-line 64-byte `Order` was not built.

## Core isolation and pinning

**What and why.** An OS-scheduled thread can be preempted or migrated at any time: cold caches and an unbounded delay that show up as tail latency. The design isolates cores 4-7 (`isolcpus`, `nohz_full`, `rcu_nocbs`, `performance` governor, IRQs moved off by `scripts/tune_host.sh`) and pins one thread to each of cores 4 and 5. `pin_to_core` sets a one-core mask with `pthread_setaffinity_np`; `verify_affinity` reads it back and throws unless it is exactly one core. `run_preflight_checks()` verifies isolation, governor and no SMT sibling per core (cores 4-7 are E-cores without SMT; cores 2/3 are siblings of one P-core, so choose and verify cores on any other machine).

**What the ablation row measures.** The `unpinned` build only removes the two affinity calls; the threads then run on whichever CPUs the scheduler picks on a busy desktop. Final run, price-time P99.9: baseline 1,148, unpinned 2,435 (trials 52,559 / 69,960 / 2,435 / 1,682 / 1,161), PROVEN, but only by a 4 ns margin (fastest unpinned trial 1,161 vs slowest baseline trial 1,157) over a 1,161-69,960 ns range; pro-rata 1,782 vs 2,343, ABLATED_FASTER. The unpinned build had the lowest P50 (248 / 251 ns vs 320 / 326), most likely because it landed on faster P-cores. That is placement luck, not an advantage of leaving threads unpinned. In an earlier session the same variant gave P99.9 up to 144,646 ns per trial, and the stock unpinned build of the pinning experiment below was bimodal. The stock-unpinned row has read INCONCLUSIVE, INCONCLUSIVE/ABLATED_FASTER and PROVEN/ABLATED_FASTER in successive sessions: it flips with desktop load, so it must not be used as evidence for pinning. The harness's preflight does not read back the actual CPU mask, and `meta.json` prints the configured cores 4/5 even for this variant.

**The factorial** (one-off experiment at commit `9c5813f`: 14 interleaved rounds per condition, price-time P99.9 median over rounds; **not reproducible from repo scripts**):

| Condition | P99.9 (ns) | vs pinned on isolated 4/5 |
|---|---|---|
| Pinned, isolated cores 4/5 | 1,588 | 1.0x |
| Pinned, non-isolated cores 8/9 | 9,652 | 6.1x (worse in 14/14 rounds) |
| Pinned, shared P-cores 0 and 2 | 6,008 | 3.8x |
| Unpinned, restricted to cores 8-11 | 12,860 | 1.33x the pinned 8/9 arm |
| Unpinned, restricted to cores 0-3 | 6,184 | 1.03x the pinned 0/2 arm |
| Unpinned, all CPUs (stock variant) | 62,433 | ~39x, bimodal: desktop load, migrations, bad cores |

Masks containing cores 4-7 are degenerate (both threads on one isolated CPU, P50 3 ms, INVALID), and 6 of 14 stock-unpinned runs migrated during clock calibration and were correctly marked INVALID. Cross-core TSC offset was measured at -14 to +39 cycles, negligible.

**Conclusion.** The row measures isolation (isolcpus, nohz_full, governor, IRQs) plus placement luck on a busy desktop, not pinning. Once the core set is fixed, pinning alone showed no measurable effect (1.0-1.3x). **Safe to say:** "isolating the latency-critical cores from the desktop improves the tail by roughly 4-40x depending on desktop load; pinning alone showed no measurable effect once the core set was fixed." **Not safe to say:** "pinning gives Nx", or that the unpinned build's lower P50 is an advantage. The factorial comes from a one-off experiment that is not reproducible from the repository. The `unpinned` row of the final run, whichever way it reads, adds nothing to it.

## Ideas tried and dropped

- **Flat open-addressing order-ID table (3 MiB).** Faster in a warm P-core microbenchmark, slower on E-cores 4-7 where the 2 MB shared L2 cannot hold it, and the tail got worse. Dropped. **Lesson:** validate on the pinned E-cores, not on whichever core a microbenchmark lands on.
- **Prefetching during sweeps.** No measurable effect. Dropped.
- **Harness prefetch of the next dataset event**, outside the measured window, kept: about -6 ns, and it only removes a harness cache miss, not engine work. Together with the ring slot and allocator it moved price-time P50 420 to 320 ns (-24%), P99 1,080 to 844, P99.9 1,420 to 1,130, and pro-rata P50 424 to 324, P99 1,292 to 1,072, P99.9 2,560 to 2,317, versus the commit before those three changes (development A/B, **not stored in the repo**).
- Not built: a one-line 64-byte `Order`.

For scale: the earlier frozen run in `results/98a597b/` reports price-time P50/P90/P99/P99.9 of 485 / 786 / 1,225 / 1,850 ns and pro-rata 491 / 826 / 1,425 / 3,132; the run of record has 320 / 549 / 851 / 1,158 (-34 / -30 / -31 / -37%) and 325 / 591 / 1,078 / 2,433 (-34 / -28 / -24 / -22%). It is a different engine and session, so that comparison is informative, not a controlled A/B.

## What a typical event costs, and what is measurement

Means over the 85,118 new orders per trial in `results_new/77eb286/{price_time,pro_rata}.raw.csv` (5 trials; rows whose queue-residence subtraction wrapped are excluded: 22 price-time, 28 pro-rata), ns:

| Component | Price-time mean | Pro-rata mean |
|---|---|---|
| Queue residence (`t_pop_start` - `t1`) | 63 | 64 |
| `pop()` | 90 | 90 |
| Matching (`match_time_ns`) | 153 | 168 |
| Fill callback (`fill_publish_ns`, 0.76 / 0.88 fills per event) | 28 | 33 |
| Unattributed (stamps between the fields) | 36 | 36 |
| End to end | 370 | 391 |

Matching is about 40% of the event. Each serialized stamp costs about 30.4 ns and is not subtracted: three or four per event, plus two per fill at about 37 ns per fill callback, put very roughly 100-150 ns of the median inside the harness's own timing. The per-event figures are therefore an upper bound on what the engine costs and are comparable across builds that use the same harness, not absolute engine latency. Pro-rata's P99.9 (2,433 vs 1,158 ns) is higher because it gives a floor share to nearly every eligible order: top-0.1% events average about 21 fills (maximum 44) against about 4 and a maximum of 8 for price-time; 80% (pro-rata) and 61% (price-time) of the slowest 0.1% of events recur in at least 4 of 5 trials, so that tail is algorithmic. The remaining stalls of 3-13 us are host noise, periodic at 1.000 ms inside noisy episodes (`CONFIG_HZ=1000`).

## What I would do next

1. Shrink `Order` to one 64-byte line and re-run the layout ablation with that, which would separate the layout change from the size change.
2. A steady-state workload generator with live cancel targets and a deep book, so cache effects have something to act on.
3. An independent reference matcher, so `reference_validation_status` stops being `NOT_RUN`.
4. Record the actual CPU mask in `meta.json` (and check it in preflight), which would have exposed the unpinned arm's placement directly.
5. Clamp `queue_residence_ns` at zero where the pop attempt began before the producer stamp.
6. A tuned-host repeat on another day. Three same-code runs on one day agreed within about 1-2%, but earlier sessions drifted 10-20%.

## Reproducing the ablations

On a tuned host ([`ALL_COMMANDS.md`](ALL_COMMANDS.md)), from a clean tree:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target blitz_lob blitz_gen_dataset -j"$(nproc)"
./build/blitz_gen_dataset --seed 42 --count 110000 --output datasets/bench_110k.bin
./scripts/run_ablations.sh --trials 5
```

The script builds the baseline and the four variants, regenerates the flat-layout dataset, runs both policies (10 runs), and writes `results/<hash>/ablation_summary.{csv,md}` plus each run's CSV, `.raw.csv` and `.meta.json` (set `RESULTS_ROOT` to write elsewhere). Expect the same direction for the PROVEN rows but not identical multipliers on another machine or session; the NO_MEASURABLE_EFFECT and unpinned rows should not be expected to reproduce at all. Reconfigure CMake after every commit before benchmarking, since the commit stamp is taken at configure time. The dataset flags are in `datasets/manifest.txt`.
