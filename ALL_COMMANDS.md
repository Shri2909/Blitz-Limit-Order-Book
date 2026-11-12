# Command Reference

The exact commands for building, testing, tuning a host, generating datasets, running the benchmarks and
regenerating the results and charts. Run from the repository root. **[ROOT]** marks commands that need
administrator rights. Inspect the host before tuning: service names, IRQ routing, boot configuration, core ids and
NUMA nodes vary by machine.

The examples use `RX_CORE_ID=4` (producer) and `MATCHING_CORE_ID=5` (matching thread) from
`include/hydra/config.hpp`, which are the development host's E-cores, and NUMA node 0. They are compile-time
constants: for other hardware change them and rebuild. Running these commands measures the checked-out revision.
It will not reproduce the figures in [CONSOLIDATED_BENCHMARK_RESULTS.md](CONSOLIDATED_BENCHMARK_RESULTS.md) exactly:
the same binary's tail percentiles can move 10 to 20% between sessions.

Contents: [1 Build, test, format](#1-build-test-format) ·
[2 Inspect the host](#2-inspect-the-host) ·
[3 Tune the host](#3-tune-the-host-root) ·
[4 Verify tuning](#4-verify-tuning) ·
[5 Datasets](#5-datasets) ·
[6 Benchmarks](#6-benchmarks) ·
[7 Results, verdicts and charts](#7-results-verdicts-and-charts) ·
[8 Sanitizer builds](#8-sanitizer-builds) ·
[9 A typical session](#9-a-typical-session)

---

## 1. Build, test, format

Build targets use the `blitz_` prefix (the C++ namespace is `hydra::`).

```bash
# Configure and build the default targets: blitz_lob, blitz_lob_asan, blitz_lob_tsan,
# blitz_gen_dataset and the ten test executables
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

**Reconfigure CMake after every commit, before benchmarking.** The git hash and the dirty flag are stamped into
the binaries when CMake configures, not when it compiles. If you commit and only rebuild, the results report the
old commit (the repeat run in [CONSOLIDATED_BENCHMARK_RESULTS.md](CONSOLIDATED_BENCHMARK_RESULTS.md) was stamped
`a27e77f` for exactly this reason). Re-running the `cmake -S . -B build ...` line above is enough.

```bash
# Ten test targets: phase1_types, phase2_timing, phase3_pool, phase4_spsc, phase5_orderbook,
# phase6_matcher, phase7_pipeline, phase8_benchmark, phase9_suite, phase10_verdict
ctest --test-dir build --output-on-failure
ctest --test-dir build -N                                      # list them
ctest --test-dir build -R phase6_matcher --output-on-failure   # a single suite
./build/blitz_lob_test_phase6_matcher                          # or run the executable directly

# Python tests (two files: test_chart_reporting.py, test_verdict_parity.py)
python3 -m unittest discover -s tests -p 'test_*.py'

# CLI help (both are safe to run)
./build/blitz_lob --help
./build/blitz_gen_dataset --help
./build/blitz_lob --test            # prints the standalone test commands
```

The tests check behavior and do not measure latency. `phase7_pipeline` and `phase8_benchmark` run threads
in-process, so do not run `ctest` while a benchmark is running on the same host.

Formatting (versions are pinned in `requirements-dev.txt`: clang-format 21.1.8, black 25.1.0):

```bash
python3 -m pip install -r requirements-dev.txt -r charts/requirements.txt
./scripts/format.sh --check          # clang-format over the tracked .cpp/.hpp/.c/.h files; fails on any diff
./scripts/format.sh --write          # apply it in place
python3 -m black --check charts/generate_charts.py tests/test_chart_reporting.py
```

`scripts/format.sh` refuses to run unless `clang-format` reports exactly 21.1.8; set `CLANG_FORMAT=/path/to/clang-format`
to use a specific binary. The GitHub workflow (`.github/workflows/format.yml`) runs the two `--check` commands
above and nothing else: CI does not build the code or run the tests.

Running `./build/blitz_lob` with no flags starts the live synthetic pipeline until Ctrl+C. It is a smoke test and
prints no latency numbers.

---

## 2. Inspect the host

Read-only. Run these first to learn the real core numbers and whether tuning is needed.

```bash
lscpu
nproc
cat /proc/cmdline                                            # are isolcpus / nohz_full / rcu_nocbs present?

# SMT sibling lists: a usable pinned core is its own sole sibling
for c in /sys/devices/system/cpu/cpu*/topology/thread_siblings_list; do
    echo "$c: $(cat "$c")"
done

cat /sys/devices/system/cpu/isolated                         # should include 4-5 (4-7 on the development host)
cat /sys/devices/system/cpu/cpu4/cache/index2/shared_cpu_list   # cores sharing cpu4's L2 (4-7 on the E-core cluster)
cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor    # must be "performance"
cat /sys/devices/system/cpu/cpu5/cpufreq/scaling_governor
numactl --hardware                                           # which node owns cores 4/5
cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo "not intel_pstate"
cat /proc/sys/kernel/nmi_watchdog
systemctl is-enabled power-profiles-daemon                   # want: masked or masked-runtime
systemctl is-active irqbalance                               # want: inactive

ps -eLo pid,psr,pcpu,comm | awk '$2==4 || $2==5' | sort -k3 -nr | head -20
ps aux --sort=-%cpu | head -20
```

On the development host (12th-gen i5-1235U, hybrid) cores 0-3 are two P-cores with SMT, and cores 4-11 are E-cores
with no SMT sibling. Cores 4-7 are one E-core cluster sharing a 2 MB L2. Check `thread_siblings_list` on your own
machine and pick two cores that are each their own sole sibling.

---

## 3. Tune the host [ROOT]

### 3.1 Boot parameters (once, then reboot)

Add `isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7` to the kernel command line without duplicating existing entries.
`isolcpus` removes the cores from the scheduler pool, `nohz_full` suppresses the periodic tick on them, and
`rcu_nocbs` moves RCU callbacks off them. The whole E-core cluster 4-7 is isolated (not just the two pinned cores)
so that work on cores 6 and 7 cannot pollute the shared L2.

```bash
sudoedit /etc/default/grub          # append to GRUB_CMDLINE_LINUX_DEFAULT
sudo update-grub
sudo reboot
cat /proc/cmdline                   # after the reboot: the three parameters must be present
```

### 3.2 Runtime tuning (after every reboot)

```bash
sudo scripts/tune_host.sh                # pinned cores default to "4,5"
sudo scripts/tune_host.sh 4,5,6,7        # same, for a custom list (see below)
```

What the script does, in order (all of it resets on reboot, so repeat it after each one):

1. Masks `power-profiles-daemon` for this boot (`systemctl mask --runtime --now`). It is D-Bus activated and
   resets governors, so stopping it is not enough; it must be masked.
2. Stops `irqbalance`, `cron`, `atd` and `unattended-upgrades`.
3. Sets the `performance` governor on the listed cores, with the `cpupower` syntax
   `cpupower -c 4,5 frequency-set --governor performance`, and `performance` energy preference where the CPU has one.
4. Writes `0` to `/proc/sys/kernel/nmi_watchdog`.
5. Moves every movable device IRQ to the cores that are not in the list. Some managed IRQs (NVMe) refuse the write;
   that is expected, and the script prints what is left on the pinned cores.
6. Prints the isolated set, each listed core's governor, SMT siblings and shared-L2 list.

The argument is the list of cores to keep quiet. The default `4,5` keeps device IRQs off the two pinned cores
only; passing `4,5,6,7` also keeps them off cores 6 and 7 and sets their governor. Close browsers and other
CPU-heavy applications first: load on the other cores still affects the shared turbo and thermal budget.

Equivalent manual steps, if you prefer not to run the script:

```bash
sudo systemctl mask --runtime --now power-profiles-daemon    # stop alone is not enough
sudo systemctl stop irqbalance
sudo cpupower -c 4,5 frequency-set --governor performance
# without cpupower:
echo performance | sudo tee /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor
echo performance | sudo tee /sys/devices/system/cpu/cpu5/cpufreq/scaling_governor

# Move device IRQs off the pinned cores: use every other CPU number on your host
# (0-3,6-11 on the 12-thread development host)
for f in /proc/irq/[0-9]*/smp_affinity_list; do
    echo 0-3,6-11 | sudo tee "$f" >/dev/null 2>&1
done
```

Optional and not used by the script: `echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo` removes
turbo (less jitter, lower peak clock).

---

## 4. Verify tuning

```bash
cat /proc/cmdline | tr ' ' '\n' | grep -E 'isolcpus|nohz_full|rcu_nocbs'   # expect the three 4-7 parameters
cat /sys/devices/system/cpu/isolated                              # expect: 4-7 (at least 4-5)
cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor         # expect: performance
cat /sys/devices/system/cpu/cpu5/cpufreq/scaling_governor         # expect: performance
cat /sys/devices/system/cpu/cpu4/topology/thread_siblings_list    # expect: 4
cat /sys/devices/system/cpu/cpu5/topology/thread_siblings_list    # expect: 5
cat /sys/devices/system/cpu/cpu4/cache/index2/shared_cpu_list     # expect: 4-7
systemctl is-enabled power-profiles-daemon                        # expect: masked-runtime (or masked)
systemctl is-active irqbalance                                    # expect: inactive
cat /proc/sys/kernel/nmi_watchdog                                 # expect: 0
grep -H . /proc/irq/*/effective_affinity_list | grep -E ':(4|5)$' # expect little or nothing (managed IRQs may stay)
ps -eLo pid,psr,pcpu,comm | awk '$2==4 || $2==5'                  # expect: empty or idle
```

The benchmark's preflight (`run_preflight_checks()` in `src/benchmark.cpp`) enforces, for each pinned core,
isolation membership (the core is listed in `/sys/devices/system/cpu/isolated`), the `performance` governor, and no
SMT sibling, and refuses to run otherwise. It does not check `nohz_full`, `rcu_nocbs`, IRQ placement, NUMA binding,
the shared-L2 cluster or other processes, so confirm those yourself with the commands above.

---

## 5. Datasets

`blitz_gen_dataset` flags other than `--output` default to the canonical values from `config.hpp`, so the short
form below equals the explicit one. Output is deterministic for a given flag set. The flags of every dataset used
are recorded in `datasets/manifest.txt` (the `.bin` files are gitignored; a generator run prints a ready-to-paste
manifest entry). The benchmark needs at least 110,000 events (10,000 warmup + 100,000 measured).

```bash
mkdir -p datasets

# Canonical mixed workload: 110,000 events, seed 42 (hash 51ce6285b274413e)
./build/blitz_gen_dataset --seed 42 --count 110000 --output datasets/bench_110k.bin

# Explicit equivalent
./build/blitz_gen_dataset --seed 42 --count 110000 --mid-price 100000 \
    --spread 500 --min-qty 1 --max-qty 1000 --cancel-ratio 0.15 \
    --ioc-ratio 0.05 --fok-ratio 0.02 --rate-hz 100000 \
    --output datasets/bench_110k.bin
```

Flat-layout ablation dataset. That variant's `Order` is 88 bytes, so its on-disk record size differs and it needs
its own generator build and file (replaying `bench_110k.bin` against it would desynchronize the record stream).
`scripts/run_ablations.sh` generates its own copy in the results directory, so this is only needed for manual runs:

```bash
cmake --build build --target blitz_gen_dataset_ablation_flat_layout -j"$(nproc)"
./build/blitz_gen_dataset_ablation_flat_layout --seed 42 --count 110000 \
    --output datasets/ablation4_flat.bin
```

Other generator flags: `--replace-ratio F` (default 0) and `--client-id-count N` (default 8; a smaller value raises
the self-trade rate, for example `--client-id-count 2`). The generator rejects invalid parameter combinations
before writing anything. With the current generator the two commands above reproduce, byte for byte, the datasets
used for the run of record.

---

## 6. Benchmarks

Never run a benchmark on an untuned host: the preflight refuses, and a run with background load is not citable.
The benchmark pins itself to the configured cores, so prefix every run with `numactl` to bind the memory node.
Runs with fewer than 5 trials are marked as smoketests and are never citable.

Use a clean tree for citable results. For a deliberate experiment with uncommitted changes, reconfigure and rebuild
(the dirty flag is also taken at configure time, over tracked files only) and pass `--allow-dirty`: the output
still records the dirty state, and such a run must not be cited as a measurement of a clean commit. Without
`--allow-dirty` a binary built from a dirty tree produces runs marked invalid.

Before benchmarking, after the last commit:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release       # re-stamps the commit hash
cmake --build build -j"$(nproc)"
```

Write new results to `results_new/<commit>/` (or any other directory). `results/` holds the frozen earlier run
and is read-only by convention.

### Canonical runs

```bash
commit="$(git rev-parse --short HEAD)"
mkdir -p "results_new/$commit"

numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob --benchmark --mode price_time --trials 5 \
    --dataset datasets/bench_110k.bin --output "results_new/$commit/price_time.csv"

numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob --benchmark --mode pro_rata --trials 5 \
    --dataset datasets/bench_110k.bin --output "results_new/$commit/pro_rata.csv"
```

Add `--verbose` for workload, pool and queue diagnostics in the console report. Each run writes `<name>.csv`,
`<name>.raw.csv` and `<name>.meta.json` next to `--output` (see
[`METHODOLOGY.md`](METHODOLOGY.md#output-files)). The console report is printed to stdout; redirect it
(`... | tee "results_new/$commit/price_time.console.txt"`) if you want to keep it.

Flags: `--benchmark` (needs `--output` and `--dataset`), `--mode price_time|pro_rata` (default `price_time`),
`--trials N` (default 5), `--verbose`, `--allow-dirty`.

### Ablation suite (baseline plus four variants, both policies, 10 runs)

```bash
RESULTS_ROOT=results_new ./scripts/run_ablations.sh --trials 5
./scripts/run_ablations.sh --trials 5            # without RESULTS_ROOT: writes to results/<commit>/
RESULTS_ROOT=results_new ./scripts/run_ablations.sh --trials 5 --allow-dirty   # dirty tree, not citable
```

The script requires `datasets/bench_110k.bin` and a configured `build/` directory. It builds the four variant
binaries and the flat-layout generator, writes `ablation4_flat.bin` into the results directory, runs the baseline
(`./build/blitz_lob`) and the four variants for each mode under `numactl --cpunodebind=0 --membind=0`, and writes
`<root>/<commit>/ablation_<variant>_<mode>.{csv,raw.csv,meta.json}`, `ablation_raw.jsonl` (per-run summaries, one
line each, including the per-trial P99.9 values) and `ablation_summary.{csv,md}` with the verdicts PROVEN,
NO_MEASURABLE_EFFECT, ABLATED_FASTER, INCONCLUSIVE or INVALID ([rule](METHODOLOGY.md#ablation-verdicts)). A failed
run is recorded as INVALID and the suite continues. The ablation baseline is a separate run from the canonical
runs above, even though it is the same binary; compare variants only with the baseline of the same suite.

One variant by hand (build it first):

```bash
cmake --build build --target blitz_lob_ablation_mutex_queue -j"$(nproc)"
numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob_ablation_mutex_queue --benchmark --mode price_time --trials 5 \
    --dataset datasets/bench_110k.bin --output /tmp/mutex_queue_pt.csv
```

Variants are `blitz_lob_ablation_{mutex_queue,unpinned,malloc_pool,flat_layout}`. The flat-layout one takes
`--dataset datasets/ablation4_flat.bin` (generated in [section 5](#5-datasets)).

### Smoketest run (never citable)

```bash
numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob --benchmark --mode price_time --trials 1 \
    --dataset datasets/bench_110k.bin --output /tmp/smoketest.csv
```

---

## 7. Results, verdicts and charts

```bash
ls "results_new/$commit/"

# Check that both canonical runs are valid and deterministic before citing them
python3 - "results_new/$commit" <<'PY'
import csv, sys
for name in ("price_time.csv", "pro_rata.csv"):
    rows = list(csv.DictReader(open(f"{sys.argv[1]}/{name}")))
    print(name,
          "valid=" + str(all(r["valid"] == "1" for r in rows)),
          "deterministic=" + str(all(r["determinism_status"] == "PASS" for r in rows)),
          "dropped=" + str(sum(int(r["dropped_events"]) for r in rows)))
PY
```

P99.99 is not in the CSVs; recompute it from the `.raw.csv` files with the snippet in
[METHODOLOGY.md](METHODOLOGY.md#recomputing-a-percentile-from-raw-samples). It is indicative only (about 9 tail
samples per trial).

### Verdict classifier

`scripts/ablation_verdict.py` is a module (there is no command line for it). `run_ablations.sh` imports it, and
`tests/verdict_cases.json` runs the same cases through it and through `hydra::stats::classify_ablation` in
`include/hydra/stats.hpp`. To re-classify an existing suite from `ablation_raw.jsonl`:

```bash
python3 - results_new/<commit> <<'PY'
import json, sys
sys.path.insert(0, "scripts")
from ablation_verdict import classify, verdict_text

rows = {(r["label"], r["mode"]): r for r in map(json.loads, open(f"{sys.argv[1]}/ablation_raw.jsonl"))}
for mode in ("price_time", "pro_rata"):
    for label in ("mutex_queue", "unpinned", "malloc_pool", "flat_layout"):
        print(mode, label, verdict_text(classify(rows[("baseline", mode)], rows[(label, mode)])))
PY
```

`classify(baseline, ablated)` takes two dicts shaped like the lines of `ablation_raw.jsonl` (`ok`, `valid`,
`determinism_ok`, the three zero-count fields and `p999_trials`) and returns the bare verdict code.

### Charts

```bash
# Regenerate the README charts (overwrites the PNGs in charts/)
python3 charts/generate_charts.py \
    --results-dir results_new/<commit> \
    --baseline-dir results/98a597b \
    --ctest-log build/Testing/Temporary/LastTest.log

python3 charts/generate_charts.py --help
```

Options: `--results-dir` (the run of record; default `results_new/7c2fd2c`), `--baseline-dir` (the earlier run used
by the before/after chart; default `results/98a597b`), `--out-dir` (where the PNGs go; default `charts/`) and
`--ctest-log` (the CTest log for the tests tile; without it the tile shows N/A). `LastTest.log` is written by
`ctest --test-dir build`, so run the full test suite first. To preview without touching the committed images, add
`--out-dir /tmp/charts_preview`. The charts need `matplotlib` (`charts/requirements.txt`).

---

## 8. Sanitizer builds

`blitz_lob_asan` (ASan + UBSan) and `blitz_lob_tsan` (TSan) are full-binary sanitizer builds of the engine, built
by default; they are separate because the two runtimes cannot be combined. The test targets `phase3_pool`,
`phase6_matcher` and `phase9_suite` are built with ASan + UBSan and `phase4_spsc` with TSan, so `ctest` already
exercises them.

```bash
# Bounded live-pipeline smoke run under each sanitizer (SIGINT after 10 s)
timeout -s INT 10 ./build/blitz_lob_asan
timeout -s INT 10 ./build/blitz_lob_tsan

# Benchmark path under each sanitizer, both policies, 1 trial. Slow (-O1 plus instrumentation),
# marked as a smoketest and the latencies are meaningless: this checks for races and memory errors only.
# The preflight still needs a tuned host.
for mode in price_time pro_rata; do
    ./build/blitz_lob_asan --benchmark --mode "$mode" --trials 1 \
        --dataset datasets/bench_110k.bin --output "/tmp/asan_$mode.csv"
    ./build/blitz_lob_tsan --benchmark --mode "$mode" --trials 1 \
        --dataset datasets/bench_110k.bin --output "/tmp/tsan_$mode.csv"
done
```

---

## 9. A typical session

Everything above in order, for a fresh benchmark of the current commit (after a reboot):

```bash
sudo scripts/tune_host.sh                                   # section 3.2
cat /sys/devices/system/cpu/isolated                         # section 4: confirm before measuring

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release               # reconfigure: re-stamps the commit
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

[ -f datasets/bench_110k.bin ] || ./build/blitz_gen_dataset --seed 42 --count 110000 --output datasets/bench_110k.bin

commit="$(git rev-parse --short HEAD)"
mkdir -p "results_new/$commit"
for mode in price_time pro_rata; do
    numactl --cpunodebind=0 --membind=0 -- \
        ./build/blitz_lob --benchmark --mode "$mode" --trials 5 \
        --dataset datasets/bench_110k.bin --output "results_new/$commit/$mode.csv" \
        | tee "results_new/$commit/$mode.console.txt"
done
RESULTS_ROOT=results_new ./scripts/run_ablations.sh --trials 5 | tee "results_new/ablation_run_$commit.log"

python3 charts/generate_charts.py --results-dir "results_new/$commit" \
    --ctest-log build/Testing/Temporary/LastTest.log
```
