# HYDRA-LOB Design

This is the canonical design document for this codebase's performance-critical
decisions: the reasoning behind each one, backed by whatever data actually
exists for it today. It is not a changelog — there is no prior version of
this system to evolve from (git history is a single scaffold commit; several
of the files discussed below have no history at all). Four of the five
before/after figures below come from ablation experiments run this session:
each rejected alternative was temporarily built in place of the real
implementation, benchmarked against the same matched dataset, then reverted
— not a permanent branch or variant kept in the tree. The fifth (AF_XDP) has
no benchmark integration to ablate against at all yet, and is marked as such.

**These four ablations are now reproducible with one command,
`./scripts/run_ablations.sh`**, not just by hand-patching source as
described below. Each rejected alternative lives permanently in its own
directory under `ablation/` (never mutating `src/`/`include/`) and builds
as a separate, not-built-by-default CMake target that shadows exactly one
real header via include-path ordering — see `ablation/mutex_queue/include/
hydra/spsc_queue.hpp`'s own comment for the mechanism, and README.md's
"Extended benchmark suite" section for the full command list. The
hand-patch/revert steps under "Reproducing these numbers" below are kept
as an explanation of what each ablation *is*, not as the recommended way
to reproduce the numbers anymore.

**Current measured baseline** (the reference every ablation below is
compared to): replaying the pinned dataset described in "Reproducing these
numbers," across 5 trials — **median P99: 1222.0 ns**, **median P99.9:
1762.0 ns**, spread (max−min) of P99.9 across trials: **137.0 ns** —
measured 2026-07-21 at commit `98a597b`, clean working tree
(`dirty=false`, confirmed against `git status`, not merely self-reported).
Raw evidence: `results/98a597b/ablation_baseline_price_time.csv` /
`.meta.json`; consolidated in `CONSOLIDATED_BENCHMARK_RESULTS.md`. This
supersedes an earlier version of this baseline (819.0 ns / 1205.0 ns) that
was measured against an uncommitted working tree with no traceable commit
— that number should no longer be cited. P99.9 is the figure each ablation
table below compares against; P99 is included here since it's the number
`--benchmark` prints directly to its own console output, unlike P99.9
which requires reading the CSV.

Two of the four ablation verdicts below changed materially between that
earlier run and this one — core pinning and cache-line layout are now
**INCONCLUSIVE under price-time matching**, not the clean proofs previously
reported. This isn't a sign the earlier conclusions were fabricated so much
as a reminder that a single 5-trial run of a noise-sensitive ablation
(especially "unpinned") can land anywhere; see each section below for the
specific numbers and the pro-rata cross-check.

---

### Lock-free SPSC queue (RX → matching handoff)

**Problem.** The RX thread and the matching thread hand off one `Order`
per incoming event across a boundary that executes on every single order —
whatever mechanism does that handoff is on the hot path unconditionally. A
buffer shared by two threads needs synchronization; get it wrong and you
either tear a 128-byte `Order` mid-read/write, or introduce a stall every
time one thread holds a lock the other needs.

**Rejected alternative.** A `std::mutex`-guarded `std::queue<Order>`. Under
contention, the losing thread blocks and is parked by the OS scheduler — a
futex wait/wake round trip costs on the order of hundreds of nanoseconds to
low microseconds on a modern kernel. It also reintroduces exactly the kind
of scheduler-mediated latency the CPU-isolation work below exists to
eliminate — a mutex can put the matching thread to sleep regardless of how
carefully its core is pinned.

**Measured before/after.** `include/hydra/spsc_queue.hpp`'s entire internal
implementation was temporarily replaced with a `std::mutex` + `std::queue<T>`
behind the identical public `push()`/`pop()`/`capacity()`/`approx_size()`
contract, rebuilt, and benchmarked against the same `bench_110k.bin`
(seed 42) — the mutex/queue variant never touches `Order`'s on-disk layout,
so no new dataset was needed. Reverted immediately after measuring.

| | Median P99 | Median P99.9 | Spread (max−min, P99.9) |
|---|---|---|---|
| Lock-free SPSC (current) | 1222.0 ns | **1762.0 ns** | 137.0 ns |
| `std::mutex` + `std::queue` | 5484.0 ns | 7770.0 ns | 915.0 ns |
| **Delta** | **~4.49x worse** | **~4.41x worse** | **~6.68x worse** |

Verdict: **PROVEN** — consistent in direction and comfortably outside
trial-to-trial noise on both matching policies (pro-rata: 3,065.0 ns →
7,774.0 ns P99.9, ~2.54x worse; `results/98a597b/ablation_summary.md`).

**Current implementation notes.** `include/hydra/spsc_queue.hpp:25-27`:
`buffer_`, `tail_`, and `head_` are each independently `alignas(64)` — the
producer-owned `tail_` and consumer-owned `head_` counters sit on separate
cache lines so a write to one never invalidates the other thread's line
(false-sharing avoidance). `push()` (line 48) and `pop()` (line 80) each do
one relaxed load of the caller's own counter plus one acquire load of the
other thread's counter — the minimum synchronization the release/acquire
pairing needs for correctness, no CAS loop, no lock. Capacity
(`hydra::SPSC_CAPACITY`, `config.hpp:60`) is fixed at 65536 and enforced
power-of-2 via `static_assert`, so slot indexing is a bitmask, never a
division.

---

### Core pinning / CPU isolation

**Problem.** An OS-scheduled thread can be preempted or migrated to a
different core at any point the kernel chooses, not just when the
programmer wants it to be. A migration costs a cold cache and TLB on the
new core; a preemption inserts an arbitrary, unbounded delay into whatever
was being measured. Either one turns a "should take ~100ns" operation into
an occasional multi-microsecond outlier that shows up as unexplained tail
latency.

**Rejected alternative.** Unpinned, OS-scheduled threads (the default for
any `std::thread` with no explicit affinity call). This class of problem was
also directly observed this session, not just theorized: an early choice of
core IDs (2 and 3) turned out to be the two SMT (hyperthread) siblings of a
single physical core on the development machine — the two "pinned" threads
would have shared L1i/decoders/execution ports even with affinity set,
reintroducing exactly the contention isolation is meant to prevent.
`RX_CORE_ID`/`MATCHING_CORE_ID` were moved to 4/5 (E-cores with no SMT
sibling on that CPU) as a direct result.

**Measured before/after.** `pin_to_core()`/`verify_affinity()` were
temporarily commented out at both call sites that matter for `--benchmark`
(`src/benchmark.cpp`'s `run_benchmark()` and `src/pipeline.cpp`'s
`matching_thread_fn()`), rebuilt, and benchmarked against the same
`bench_110k.bin`. System-level preflight checks (`isolcpus`, governor, SMT)
were left in place and still passed — this ablation is specifically about
whether *this process's threads* are pinned, not about the system-level
tuning around them. Reverted immediately after measuring.

| | Median P99 | Median P99.9 | Spread (max−min, P99.9) |
|---|---|---|---|
| Pinned (current) | 1222.0 ns | **1762.0 ns** | 137.0 ns |
| Unpinned | 1098.0 ns | 9216.0 ns | 139917.0 ns |
| **Delta** | **~0.90x (lower — noise)** | **~5.23x worse** | **~1021x worse** |

Verdict: **INCONCLUSIVE under price-time.** Unpinned's own *median P99*
comes in *lower* than pinned's — the effect only shows up in the P99.9
tail, and that tail measurement itself swings across five orders of
magnitude within this one ablation's 5 trials (4,729 ns to 144,646 ns
P99.9, on the identical binary). A median-of-5 comparison can't cleanly
separate "pinning helps" from "unpinned happened to get an unlucky
scheduler trial" at that noise level.

This is not a failure of the pinning hypothesis — it's exactly the
instability core pinning exists to remove, observed directly. The same
ablation under **pro-rata matching is clean: PROVEN, ~1.60x worse**
(3,065.0 ns → 4,906.0 ns P99.9; `results/98a597b/ablation_summary.md`),
which is the more trustworthy signal for this design decision until a
price-time run with more trials (or a longer measurement window) resolves
the tail noise. The spread explosion (~1021x under price-time) is itself
the more telling number regardless of policy: an unpinned thread's tail
latency is dominated by *when* the scheduler happens to migrate or preempt
it, which varies enormously run to run — one trial in this ablation hit
144,646 ns P99.9 in isolation, others as low as 4,729 ns.

**Current implementation notes.** `include/hydra/affinity.hpp`:
`pin_to_core()` (line 40) calls `pthread_setaffinity_np` with a single-core
`cpu_set_t`; `verify_affinity()` (line 54) then reads back
`sched_getaffinity()` and throws with a detailed diagnostic if the mask
isn't *exactly* one core — a silent partial failure (e.g. affinity request
ignored) is treated as fatal, not logged-and-continued. Both
`rx_thread_fn` and `matching_thread_fn` (`src/pipeline.cpp`) call this pair
before entering their hot loop. `src/benchmark.cpp`'s
`run_preflight_checks()` independently verifies, before any measurement
starts, that both pinned cores are actually present in
`/sys/devices/system/cpu/isolated`, that their cpufreq governor is
`performance`, and that neither has an SMT sibling — the benchmark refuses
to run rather than produce a number from an unverified environment (see
`README.md`'s Environment Requirements section, itself mirrored from
`config.hpp`).

---

### Object pooling (Order/Level allocation)

**Problem.** A matching engine processing one order at a time cannot afford
a general-purpose allocator call (`new`/`malloc`) per order: `malloc`
implementations take internal locks or use per-thread arenas with their own
bookkeeping overhead, and allocation/deallocation patterns driven by
arbitrary order arrival can fragment the heap over a long-running process,
degrading allocator performance over time in a way that's hard to bound or
predict.

**Rejected alternative.** Per-order `new`/`delete` for `Order` objects.
Beyond lock/arena overhead, this also has no natural way to bound
worst-case latency — allocator behavior under fragmentation is not
something this system's preflight/isolation model can verify or enforce
the way core affinity can.

**Measured before/after.** `include/hydra/object_pool.hpp`'s `acquire()`/
`release()` were temporarily rewritten to call `::new(std::nothrow) T(...)`
/ `delete ptr` directly, bypassing the free-list slab entirely (the slab
and free-list members stayed declared but unused), rebuilt, and benchmarked
against the same `bench_110k.bin` — this ablation only changes how `Order`
memory is *obtained*, never its layout, so no new dataset was needed.
Reverted immediately after measuring.

| | Median P99 | Median P99.9 | Spread (max−min, P99.9) |
|---|---|---|---|
| Object pool (current) | 1222.0 ns | **1762.0 ns** | 137.0 ns |
| `new`/`delete` per order | 2167.0 ns | 6079.0 ns | 850.0 ns |
| **Delta** | **~1.77x worse** | **~3.45x worse** | **~6.20x worse** |

Verdict: **PROVEN** — consistent on both matching policies (pro-rata:
3,065.0 ns → 6,948.0 ns P99.9, ~2.27x worse;
`results/98a597b/ablation_summary.md`).

One additional real characteristic of the *current* design (not a
before/after — there is no "before" data point for this one) is proven
directly in the test suite, not a separate benchmark: `tests/
test_phase5_orderbook.cpp`'s `test_cancel_is_position_independent_micro_benchmark`
(head vs. tail position within one fixed-depth FIFO) and
`test_cancel_latency_is_depth_independent` (depth 100 vs. 40,000) both
assert cancel cost stays flat rather than scaling with depth, consistent
with the O(1) cancel design described below.

**Current implementation notes.** `include/hydra/object_pool.hpp`:
`ObjectPool<T,N>` (line 23) holds one `alignas(64) std::array<Slot,N>`
slab embedded inline (heap-allocated once via `std::make_unique` at
pipeline construction, never a stack local — see the file's own top-of-file
warning) and an intrusive free-list threaded through a `union Slot { T obj;
Slot *next; }` (line 39). `acquire()` (line 69) and `release()` (line 112)
are plain pointer swaps — O(1), no allocation, no atomics: the file's own
comment (line 28-38) documents this as deliberate given the single-owner-
per-pool-instance invariant the pipeline wiring guarantees (only the
matching thread ever touches a given pool), and explicitly notes what would
have to change (a tagged-pointer CAS free-list) if that invariant is ever
relaxed to multiple concurrent owners.

`include/hydra/order_book.hpp` extends the same "no general-purpose
allocator on the hot path" goal to its lookup structures: `order_index_`
(a `std::pmr::unordered_map`, line 231) and the `bids_`/`asks_` price maps
(`std::pmr::map`, lines 229-230) are backed by fixed-size
`std::pmr::monotonic_buffer_resource` arenas (lines 213-227) with an
instrumented fallback to the default heap (`arena_fallback_count()`,
line 174) so arena exhaustion is a counted, observable event rather than a
silent behavior change. `cancel_order()` (line 78) does an O(1) hash lookup
by `order_id` to get direct `Order*`/`Level*` pointers, then
`unlink_from_level()` (line 282) is a true O(1) intrusive doubly-linked-list
unlink via `prev_`/`next_` — no scan of the FIFO at any point.

---

### Cache-line padding / hot-cold field separation

**Problem.** A single flat `Order` struct with all fields adjacent means
the matching hot path — which only ever touches `order_id`, `price`, `qty`,
`side`, `tif`, and the intrusive `prev_`/`next_` FIFO pointers — pulls
audit/reporting-only fields (`timestamp_ns`, `client_id`, `client_tag`)
into cache alongside them on every access, wasting cache capacity and
bandwidth on bytes the matcher never reads.

**Rejected alternative.** A flat, unseparated `Order` layout: the same
fields, declared without the deliberate hot/cold split, `hot_padding`, or
`cold_padding`, and without `alignas(64)`. This is the same baseline the
codebase's own comment names.

**Measured before/after.** `include/hydra/types.hpp`'s `Order` was
temporarily flattened to this natural layout (see below), which shrinks
`sizeof(Order)` from 128 to 88 bytes — this **does** change `Order`'s
on-disk byte layout (it's embedded directly in `DatasetRecord`), so
replaying the existing `bench_110k.bin` against the flattened build would
have desynced the binary record stream rather than measured anything
meaningful. A fresh dataset (`ablation4_flat.bin`, identical seed 42 and
config, generated by a `blitz_gen_dataset` built against the flattened
`Order` — see `datasets/manifest.txt`) was generated and used instead, so
the logical order/cancel sequence is identical between the two runs; only
the serialized record size differs. Reverted immediately after measuring.

| | Median P99 | Median P99.9 | Spread (max−min, P99.9) |
|---|---|---|---|
| Hot/cold split, 128 bytes (current) | 1222.0 ns | **1762.0 ns** | 137.0 ns |
| Flat, unseparated, 88 bytes | 1231.0 ns | 1797.0 ns | 77.0 ns |
| **Delta** | **~1.01x worse** | **~1.02x worse** | **~0.56x (smaller)** |

Verdict: **INCONCLUSIVE**, on both matching policies (pro-rata: 3,065.0 ns
→ 3,554.0 ns P99.9, ~1.16x worse). This is a real downgrade from a
previous version of this ablation, which reported ~3.1x/~10.4x. At ~1.02x,
the effect is smaller than the ablation's *own* trial-to-trial spread
(77.0 ns), which if anything argues there's no measurable signal here at
all under the current methodology — not merely a small one. Cache-line
layout is plausibly a subtler mechanism than lock contention or allocator
overhead, and is measured here as end-to-end latency, not IPC (see below).
One transparency note that still applies regardless of magnitude: because
the flattened struct is also smaller (88 vs 128 bytes), this comparison
bundles "removing the hot/cold split" together with "reducing total struct
size" — an ablation that preserved 128 bytes while un-separating the
fields would isolate the layout variable alone, and wasn't attempted here.

Separately, `include/hydra/types.hpp:26-31`'s own comment gives a target
IPC shift ("~1.2 baseline... to ~2.1 after this split") and labels it
verbatim as "a design target and rationale, not a measured claim, until
Phase 12 records an actually-measured number against a real run." No
Phase 12 exists anywhere in this repository (the highest phase present, in
both `CMakeLists.txt` and `tests/`, is Phase 10 — AF_XDP), and `perf stat`
does not work on this machine (`perf_event_paranoid=4` blocks it, confirmed
by testing) — so that specific IPC figure remains unmeasured. **Do not
quote the 1.2→2.1 IPC figure as an achieved result anywhere**; the
1762.0ns vs 1797.0ns latency comparison above is a real, separate
measurement of the same underlying design decision, but — unlike when
this paragraph was first written — that comparison is now itself
INCONCLUSIVE (~1.02x), so it should not be substituted for the IPC claim
either. Right now neither figure supports a strong claim about this
specific optimization's payoff; the `static_assert`-enforced layout itself
is the only unconditionally true statement available.

**Current implementation notes.** `include/hydra/types.hpp:47-63`: `Order`
is `alignas(64)`, exactly 128 bytes (`static_assert` at line 65), split
into a hot 64-byte line (`order_id`, `price`, `qty`, `side`, `tif`,
`event_tag`, then 25 bytes of `hot_padding` reserved so the `prev_`/`next_`
pointers that follow land exactly on the line boundary) and a cold 64-byte
line (`timestamp_ns`, `client_id`, `client_tag[32]`, `cold_padding`). The
layout claim is the `static_assert`s themselves (`sizeof(Order) == 128`,
`alignof(Order) == 64`, and the rest, `types.hpp:65-78`) — the actual proof
mechanism, checked on every build, not a separately generated diagram that
could drift out of sync with it (see `charts/04_cache_line_layout.png` for
that diagram, hand-verified against this exact struct — 25 bytes of
`hot_padding`, not 26, and `event_tag` is a real field, both easy to get
wrong transcribing this by hand). `Level` (line 80) is separately
`alignas(64)`, exactly 64 bytes.

---

### AF_XDP kernel-bypass RX path

**Problem.** A standard socket RX path (`recvfrom()`/`recvmsg()` on an
`AF_INET`/`SOCK_DGRAM` socket) costs a syscall per receive (or per batch
with `recvmmsg`), a kernel-to-userspace copy of every packet's payload, and
full protocol-stack traversal (routing, netfilter/conntrack, socket
buffer accounting) for every packet — overhead paid even though this
system's order-entry wire protocol (`hydra::xdp::OrderWireFormat`,
30 fixed bytes) needs none of that generality.

**Rejected alternative.** The standard socket RX path described above. Its
per-packet syscall and copy cost, and its traversal of stack layers this
protocol doesn't need, sit directly on the ingestion path for every order —
in a system where the matching engine itself operates in the hundreds of
nanoseconds, a syscall-and-copy per packet would dominate end-to-end
latency.

**Measured before/after.** Not yet measured — and unlike the other four
optimizations, this one currently has no benchmark integration to measure
against at all: `--benchmark` (`src/benchmark.cpp`) replays a dataset
in-process and never spawns an AF_XDP RX thread; the live AF_XDP path
(`afxdp_rx_thread_fn`, `src/pipeline.cpp`) has no equivalent to
`--benchmark`'s trial/percentile reporting. What has been verified is
correctness, not a performance delta: over a veth pair with the peer end in
its own network namespace (`scripts/setup_veth.sh`), 15,000/15,000 sent
packets were received and parsed with 0 drops and 0 parse errors at a
moderate rate (500/sec). That is an illustrative, live-traffic correctness
result, not a controlled before/after — and veth only supports `XDP_COPY`
and generic (`skb`) attachment, not true zero-copy, so it validates
ring-plumbing and parsing correctness, not zero-copy performance (see the
WHY comment in `include/hydra/xdp/xdp_socket.hpp`).

**Current implementation notes.** `net/xdp_prog.bpf.c`: an eBPF program
redirecting UDP traffic into a `BPF_MAP_TYPE_XSKMAP` (`xsks_map`) keyed by
RX queue index, falling through to `XDP_PASS` for anything non-IPv4/UDP or
any queue with no AF_XDP socket registered. `include/hydra/xdp/xdp_socket.hpp`:
`XdpSocket` owns one UMEM (`hydra::config::AFXDP_UMEM_SIZE`, `config.hpp:145`)
plus the FILL/COMPLETION/RX/TX rings for one `{interface, queue}` pair;
`recv_batch()` (line 115) is busy-poll only — no blocking syscall on the
hot path. `include/hydra/xdp/zero_copy_parser.hpp`:
`parse_order_zero_copy()` (line 101) reads `Order` fields directly out of
the UMEM frame pointer via `reinterpret_cast` — no `memcpy` at any point
between the mapped frame and the populated `Order`.

---

## Reproducing these numbers

**Recommended: `./scripts/run_ablations.sh`** builds the baseline and all
four ablation targets and runs all five through the real, gated
`--benchmark` path automatically, writing
`results/<hash>/ablation_summary.{csv,md}` in the same shape as the table
above. The manual patch/build/measure/revert steps below explain what each
ablation target actually *is* and remain valid as a from-scratch
description, but are no longer how you'd reproduce the numbers day to day.

**The baseline and the SPSC/pinning/pooling ablations** all replay the same
matched dataset:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target blitz_lob blitz_gen_dataset -j"$(nproc)"

./build/blitz_gen_dataset --seed 42 --count 110000 --mid-price 100000 \
    --spread 500 --min-qty 1 --max-qty 1000 --cancel-ratio 0.15 \
    --ioc-ratio 0.05 --fok-ratio 0.02 --rate-hz 100000 \
    --output datasets/bench_110k.bin

# Requires the environment preflight in README.md's Environment Requirements
# section (isolcpus, performance governor, SMT off) or this refuses to run
# and tells you which check failed. Root is not required for numactl/affinity
# on this host; add sudo if your system's policy requires it.
numactl --cpunodebind=0 --membind=0 \
    ./build/blitz_lob --benchmark --mode price_time --trials 5 \
    --dataset datasets/bench_110k.bin --output results/bench.csv
```

`results/bench.csv` columns: `version,trial,iteration,latency_ns,
queue_transit_ns,match_time_ns,timestamp_unix,is_cancel`. Compute P99.9 per
trial (filtering `is_cancel=0` for the order-latency figure -- cancel
latency is a separate figure, see README.md's "Reproducible Results"
section) and take the median across trials, per `README.md`'s "Reproducible
Results" section, to reproduce the 1762.0 ns figure above.

**To reproduce an ablation**, apply the corresponding temporary patch, run
the identical command above, then revert:

- **SPSC queue**: replace `include/hydra/spsc_queue.hpp`'s internals with a
  `std::mutex` + `std::queue<T>` behind the same `push()`/`pop()`/
  `capacity()`/`approx_size()` signatures.
- **Core pinning**: comment out `pin_to_core(RX_CORE_ID)`/
  `verify_affinity(RX_CORE_ID)` in `src/benchmark.cpp`'s `run_benchmark()`,
  and `pin_to_core(MATCHING_CORE_ID)`/`verify_affinity(MATCHING_CORE_ID)`
  in `src/pipeline.cpp`'s `matching_thread_fn()`. (These used to be
  `pin_to_core(cfg.core)`/`verify_affinity(cfg.core)` — a caller-supplied
  core that could silently disagree with the core `run_preflight_checks()`
  actually validated; now hardcoded to remove that gap entirely.)
- **Object pooling**: replace `ObjectPool::acquire()`/`release()`'s bodies
  with `::new(std::nothrow) T(...)` / `delete ptr`, bypassing the free-list.

**Cache-line padding** needs its own dataset, since it changes `Order`'s
on-disk size:

```bash
# Temporarily flatten Order in include/hydra/types.hpp to:
#   struct Order {
#       uint64_t order_id; int64_t price; uint32_t qty;
#       Side side; TimeInForce tif; OrderEventTag event_tag;
#       Order *prev_; Order *next_;
#       uint64_t timestamp_ns; uint64_t client_id; char client_tag[32];
#   };
# (this must include event_tag, or the flattened struct's logical fields
# don't match the real Order and the ablation isn't measuring the same thing --
# see ablation/flat_layout/include/hydra/types.hpp for the actual variant used)
# and drop the sizeof(Order)==128 / %64==0 / alignof==64 static_asserts
# (keep is_trivially_copyable_v). Then also comment out
# dataset_generator.hpp's `static_assert(sizeof(DatasetRecord) == 256, ...)`
# -- it no longer holds once Order shrinks.

cmake --build build --target blitz_lob blitz_gen_dataset -j"$(nproc)"

./build/blitz_gen_dataset --seed 42 --count 110000 --mid-price 100000 \
    --spread 500 --min-qty 1 --max-qty 1000 --cancel-ratio 0.15 \
    --ioc-ratio 0.05 --fok-ratio 0.02 --rate-hz 100000 \
    --output datasets/ablation4_flat.bin

numactl --cpunodebind=0 --membind=0 \
    ./build/blitz_lob --benchmark --mode price_time --trials 5 \
    --dataset datasets/ablation4_flat.bin --output results/ablation4.csv
```

**Verify every revert is clean** before trusting any subsequent run:
```bash
grep -rn "ABLATION" include/ src/ tools/    # must return nothing
ctest --test-dir build --output-on-failure
```

The O(1)-cancel and struct-layout claims re-verify on any host, no
preflight tuning required, via the correctness suite:
```bash
cmake --build build --target blitz_lob_test_phase5_orderbook blitz_lob_test_phase1_types -j"$(nproc)"
./build/blitz_lob_test_phase5_orderbook   # cancel position/depth independence
./build/blitz_lob_test_phase1_types       # types.hpp static_asserts compile-checked on every build
```

**AF_XDP** has no ablation command because it has no benchmark integration
to ablate against yet — see its section above.
