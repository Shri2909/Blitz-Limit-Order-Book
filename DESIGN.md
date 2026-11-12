# Design Decisions

For each design choice: what it is, why, what was measured, what it showed, and the honest conclusion. Structure and data flow are in [`ARCHITECTURE.md`](ARCHITECTURE.md); timing, statistics and the verdict rule are in [`METHODOLOGY.md`](METHODOLOGY.md).

## Where the evidence comes from

- **Final ablation suite** (stored locally in `results_new/7c2fd2c/`, gitignored): commit `7c2fd2c`, 2026-11-12, i5-1235U with cores 4/5 isolated, 5 trials per arm, both matching policies. Each verdict compares per-trial P99.9 of a variant against the baseline of the same session ([rule](METHODOLOGY.md#ablation-verdicts)).
- **Development A/B runs** (allocator and ring-slot changes): interleaved rounds on the pinned cores, 8 rounds per mode, paired differences, faster in 8/8 rounds with an identical state hash. **These logs are not in the repo**; the numbers below are quoted from the development session and cannot be re-checked from a checkout.
- **One-off experiments** (the pinning factorial at commit `9c5813f`, a cache-simulation analysis of the layout): also not reproducible from repo scripts, and labelled each time.
- The ablation baseline is a separate run of the same binary from the canonical run quoted in the [README](README.md) (P99.9 1,170 vs 1,158 ns price-time). Both are labelled wherever they appear. Everything is in-process, synthetic-data, single-host, single-session latency.

| Choice | Alternative removed | Price-time P99.9 (baseline 1,170) | Pro-rata P99.9 (baseline 2,500) | Verdict |
|---|---|---|---|---|
| Lock-free SPSC ring | `std::mutex` + `std::queue` | 6,589 ns, 5.63x | 6,747 ns, 2.70x | PROVEN, PROVEN |
| Fixed-slab pools | `new`/`delete` per object | 4,427 ns, 3.78x | 4,112 ns, 1.64x | PROVEN, PROVEN |
| Hot/cold cache-line split | Flat 88-byte `Order` | 1,236 ns, 1.06x | 2,362 ns, 0.94x | NO_MEASURABLE_EFFECT, NO_MEASURABLE_EFFECT |
| Core isolation + pinning | Unpinned threads | 1,452 ns, 1.24x | 1,996 ns, 0.80x | INCONCLUSIVE, ABLATED_FASTER (see below: this row does not measure pinning) |

## How the ablations work

Each ablation is a separate build target whose include path puts a modified copy of exactly one header from `ablation/<name>/include/hydra/` ahead of the real one. Production sources in `src/` and `include/` are never edited, and every variant runs through the same gated `--benchmark` path.

| Target | Override | Change |
|---|---|---|
| `blitz_lob_ablation_mutex_queue` | `spsc_queue.hpp` | `std::mutex` + `std::queue<T>` behind the same API |
| `blitz_lob_ablation_unpinned` | `affinity.hpp` | `pin_to_core` / `verify_affinity` do nothing (host preflight still runs) |
| `blitz_lob_ablation_malloc_pool` | `object_pool.hpp` | `new`/`delete` per object instead of the slab |
| `blitz_lob_ablation_flat_layout` | `types.hpp`, `dataset_generator.hpp` | Flat 88-byte `Order` without the hot/cold split |

`DatasetRecord` embeds `Order`, so the flat-layout build changes the on-disk record size and replays its own file (`ablation4_flat.bin`, same seed and flags). Per-arm tables below show the median and the five per-trial P99.9 values, in ns.

## Lock-free SPSC ring

**What and why.** One `Order` crosses the producer-to-matcher boundary per event, so the handoff is on the hot path every time. A lock can park the matching thread, which undoes the isolation work.

**Implementation** (`include/hydra/spsc_queue.hpp`). The production ring is `SpscQueue<Order, 65536>`: one 64-byte slot per event whose own sequence word is the "ready" flag and shares a cache line with every field the matcher reads, so the consumer polls the line that will carry the data (one cross-core transfer) instead of a tail index plus a two-line, 128-byte copy. The slot carries `order_id`, `price`, `qty`, `side`, `tif`, `event_tag`, `client_id` and `timestamp_ns`; the book's links, `client_tag` and padding are not transported and come back zeroed (`static_assert`s fail the build if `Order` gains a field the slot does not carry). The ring shrank from 8 MiB to 4 MiB. The producer keeps a private copy of the consumer's last head, so it reads the consumer's line only when the ring looks full; the generic queue (used by the flat-layout ablation) caches both peers' indices. Indices and buffer are `alignas(64)`; there is no CAS and no lock; capacity is a power of two so indexing is a mask.

| Price-time P99.9 | Median | Trials |
|---|---|---|
| Lock-free ring (baseline) | 1,170 | 1,170 / 1,133 / 1,126 / 1,224 / 1,179 |
| Mutex + queue | 6,589 | 6,432 / 6,469 / 6,589 / 6,800 / 6,889 |

**Showed.** PROVEN under both policies: 5.63x price-time, 2.70x pro-rata (6,747 vs 2,500; trials 6,222-6,851 all above the baseline's 3,691 maximum). Median P50 is 1,209 vs 321 ns price-time. **Caveats.** The mutex arm also swaps the container (`std::queue` over a deque) and drops the slot design, so the ratio is "lock + deque vs this ring", not "lock vs lock-free in isolation". The mutex arm's tail is host-scheduling driven (a blocked consumer sleeps and wakes), which is the point of the choice but is also why the ratio depends on the machine.

**Development evidence (not stored in the repo).** Replacing the tail index plus 128-byte copy with the one-slot design: price-time P50 -61 ns, P99 -76, P99.9 -90; pro-rata -63 / -74 / -99. Caching the peer index, together with a lazy next-level lookup in sweeps and skipping a duplicate-id lookup: about -14% P50 and -7% P99 versus the previous commit.

**Conclusion.** The lock-free handoff is a clear, repeatable win on this host. The one-slot refinement is a measured but small development-session improvement, and it only applies to the 128-byte production `Order`.

## Fixed-slab object pools and the book's node allocator

**What and why.** A `new`/`malloc` per order brings allocator locks or arena bookkeeping, and arrival-driven alloc/free patterns can fragment the heap in ways that are hard to bound. `ObjectPool<T, N>` (`include/hydra/object_pool.hpp`) holds one inline `alignas(64)` slab (heap-allocated once at startup) and threads an intrusive free list through a `union Slot`. `acquire()` and `release()` are plain pointer swaps with no atomics, correct only because each pool has one owning thread. Exhaustion returns `nullptr` and is counted; it invalidates a run.

| Price-time P99.9 | Median | Trials |
|---|---|---|
| Slab pool (baseline) | 1,170 | 1,170 / 1,133 / 1,126 / 1,224 / 1,179 |
| `new`/`delete` | 4,427 | 4,427 / 4,205 / 4,476 / 4,236 / 5,538 |

**Showed.** PROVEN under both policies: 3.78x price-time, 1.64x pro-rata (4,112; trials 4,053-4,449 against baseline 2,359-3,691). P99 is 1,428 vs 849 ns price-time. **Scope.** The ablation replaces only the `Order` and `Level` pools. It says nothing about the book's lookup structures, which sit on separate allocators.

**The book's node allocator** (`order_book.hpp`). The order-ID hash map and the price-level maps are `std::pmr` containers over fixed arenas with a counted heap fallback. Their node allocator was `std::pmr::unsynchronized_pool_resource`; it is now `SizeClassFreeListResource`, one intrusive LIFO free list per 16-byte size class (up to 64 bytes; larger requests such as the hash bucket array go straight to the arena). A callgrind run measured roughly 150-175 instructions per allocate/deallocate for the pool resource against about 10 for the free list, and a freed block is reused immediately instead of the lowest free slot of a chunk. **Development evidence (not stored in the repo):** about -35% engine instructions; price-time P50 -32 ns, P99 -147, P99.9 -189; pro-rata -34 / -135 / -129. This one has no ablation target.

**Conclusion.** Pooling is a clear win and the allocator replacement was a worthwhile, repeatable step; the second claim rests on development A/B logs that are not in the repo.

## Hot/cold cache-line split (cache-line padding)

**What and why.** `Order` is `alignas(64)` and 128 bytes. Line 0 holds `order_id`, `price`, `qty`, `side`, `tif`, `event_tag`, padding and the FIFO links; line 1 holds `timestamp_ns`, `client_id`, `client_tag[32]` and padding (`include/hydra/types.hpp`, sizes `static_assert`ed). The intent: the fields a sweep needs sit together, and metadata stays out of the way, shrinking the working set of the common case.

| Price-time P99.9 | Median | Trials |
|---|---|---|
| Split, 128 B (baseline) | 1,170 | 1,170 / 1,133 / 1,126 / 1,224 / 1,179 |
| Flat, 88 B | 1,236 | 2,598 / 1,977 / 1,233 / 1,221 / 1,236 |

**Showed.** NO_MEASURABLE_EFFECT on both policies: 1.06x price-time, 0.94x pro-rata (2,362; trials 2,343-2,432). It is "not detected", not "equal": two flat price-time trials are far above baseline but the median is in band. Two confounds: the flat struct is also smaller, and the flat build uses the generic queue (a 88-byte copy and a separate tail index), which is consistent with its higher P50 (388 vs 321 ns) given the ~60 ns the one-slot ring was measured to save. IPC was not measured (`perf_event_paranoid` blocked `perf stat`).

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

**What the ablation row measures.** The `unpinned` build only removes the two affinity calls; the threads then run on whichever CPUs the scheduler picks on a busy desktop. Final run, price-time P99.9: baseline 1,170, unpinned 1,452 (trials 21,293 / 1,452 / 1,615 / 1,219 / 1,317), INCONCLUSIVE; pro-rata 1,996 vs 2,500, ABLATED_FASTER. The unpinned build had the lowest P50 (248 / 252 ns vs 321 / 324), most likely because it landed on faster P-cores. That is placement luck, not an advantage of leaving threads unpinned. In an earlier session the same variant gave P99.9 up to 144,646 ns per trial, and the stock unpinned build of the pinning experiment below was bimodal. The harness's preflight does not read back the actual CPU mask, and `meta.json` prints the configured cores 4/5 even for this variant.

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

**Conclusion.** The row measures isolation (isolcpus, nohz_full, governor, IRQs) plus placement luck on a busy desktop, not pinning. Once the core set is fixed, pinning alone showed no measurable effect (1.0-1.3x). **Safe to say:** "isolating the latency-critical cores from the desktop improves the tail by roughly 4-40x depending on desktop load; pinning alone showed no measurable effect once the core set was fixed." **Not safe to say:** "pinning gives Nx", or that the unpinned build's lower P50 is an advantage. The factorial comes from a one-off experiment that is not reproducible from the repository.

## Ideas tried and dropped

- **Flat open-addressing order-ID table (3 MiB).** Faster in a warm P-core microbenchmark, slower on E-cores 4-7 where the 2 MB shared L2 cannot hold it, and the tail got worse. Dropped. **Lesson:** validate on the pinned E-cores, not on whichever core a microbenchmark lands on.
- **Prefetching during sweeps.** No measurable effect. Dropped.
- **Harness prefetch of the next dataset event**, outside the measured window, kept: about -6 ns, and it only removes a harness cache miss, not engine work. Together with the ring slot and allocator it moved price-time P50 420 to 320 ns (-24%), P99 1,080 to 844, P99.9 1,420 to 1,130, and pro-rata P50 424 to 324, P99 1,292 to 1,072, P99.9 2,560 to 2,317, versus the commit before those three changes (development A/B, **not stored in the repo**).
- Not built: a one-line 64-byte `Order`.

For scale: the earlier frozen run in `results/98a597b/` reports price-time P99.9 1,850 ns and pro-rata 3,132. It is a different engine and session, so that comparison is informative, not a controlled A/B.

## What a typical event costs, and what is measurement

Means over the 85,118 new orders per trial in `results_new/7c2fd2c/price_time.raw.csv` (queue-residence wrap rows excluded), ns:

| Component | Price-time mean | Pro-rata mean |
|---|---|---|
| Queue residence (`t_pop_start` - `t1`) | 63 | 64 |
| `pop()` | 90 | 90 |
| Matching (`match_time_ns`) | 154 | 168 |
| Fill callback (`fill_publish_ns`, 0.76 / 0.88 fills per event) | 28 | 33 |
| Unattributed (stamps between the fields) | 36 | 36 |
| End to end | 371 | 391 |

Matching is about 40% of the event. Each serialized stamp costs about 30.4 ns and is not subtracted: three or four per event, plus two per fill at 37.4 ns per fill callback, put very roughly 100-150 ns of the median inside the harness's own timing. The per-event figures are therefore an upper bound on what the engine costs and are comparable across builds that use the same harness, not absolute engine latency. Pro-rata's P99.9 (2,380 vs 1,158 ns) is higher because it gives a floor share to nearly every eligible order: top-0.1% events average about 18 fills (maximum 44) against a price-time maximum of 8; 76% (pro-rata) and 58% (price-time) of the slowest 0.1% of events recur in at least 4 of 5 trials, so that tail is algorithmic. The remaining stalls of 3-13 us are host noise, periodic at 1.000 ms inside noisy episodes (`CONFIG_HZ=1000`).

## What I would do next

1. Shrink `Order` to one 64-byte line and re-run the layout ablation with that, which would isolate the layout change from the size change.
2. A steady-state workload generator with live cancel targets and a deep book, so cache effects have something to act on.
3. An independent reference matcher, so `reference_validation_status` stops being `NOT_RUN`.
4. Record the actual CPU mask in `meta.json` (and check it in preflight), which would have exposed the unpinned arm's placement directly.
5. Clamp `queue_residence_ns` at zero where the pop attempt began before the producer stamp.
6. A tuned-host repeat on another day, to put an actual run-to-run bound on the 10-20% session drift.

## Reproducing the ablations

On a tuned host ([`ALL_COMMANDS.md`](ALL_COMMANDS.md)), from a clean tree:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target blitz_lob blitz_gen_dataset -j"$(nproc)"
./build/blitz_gen_dataset --seed 42 --count 110000 --output datasets/bench_110k.bin
./scripts/run_ablations.sh --trials 5
```

The script builds the baseline and the four variants, regenerates the flat-layout dataset, runs both policies (10 runs), and writes `results/<hash>/ablation_summary.{csv,md}` plus each run's CSV, `.raw.csv` and `.meta.json` (set `RESULTS_ROOT` to write elsewhere). Expect the same direction for the PROVEN rows but not identical multipliers on another machine or session; the NO_MEASURABLE_EFFECT and unpinned rows should not be expected to reproduce at all. Reconfigure CMake after every commit before benchmarking, since the commit stamp is taken at configure time. The dataset flags are in `datasets/manifest.txt`.
