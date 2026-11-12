# Command Reference

The full command sequence for building, testing, tuning a host, and running
every benchmark. Run from the repository root on a dedicated benchmark host.
**[ROOT]** marks commands that need administrator rights. Inspect the host
before tuning: service names, IRQ routing, boot configuration, core IDs, and
NUMA nodes vary by machine.

The examples use `RX_CORE_ID=4` and `MATCHING_CORE_ID=5` from
`include/hydra/config.hpp` (the development host's E-cores) and NUMA node 0.
Change the constants and rebuild for other hardware. Running these commands
measures the checked-out revision; it does not reproduce the historical
`98a597b` figures in the [README](README.md).

Contents: [1 Build, test, format](#1-build-test-format) ·
[2 Inspect the host](#2-inspect-the-host) ·
[3 Reduce interference](#3-reduce-interference-root) ·
[4 Tune the CPU](#4-tune-the-cpu-root) ·
[5 Verify tuning](#5-verify-tuning) ·
[6 Datasets](#6-datasets) ·
[7 Benchmarks](#7-benchmarks) ·
[8 Results and charts](#8-results-and-charts)

---

## 1. Build, test, format

Build targets use the `blitz_` prefix (the C++ namespace is `hydra::`).

```bash
# Release build and the default targets (engine, generator, sanitizer builds, tests)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# Nine test executables; one command runs them all
ctest --test-dir build --output-on-failure
ctest --test-dir build -R phase6_matcher --output-on-failure   # a single suite

# CLI help
./build/blitz_lob --help
./build/blitz_gen_dataset --help
./build/blitz_lob --test            # prints the standalone test commands
```

Tests phases 3, 6, and 9 are built with ASan+UBSan and phase 4 with TSan.
`blitz_lob_asan` (ASan+UBSan) and `blitz_lob_tsan` (TSan) are full-binary
sanitizer builds of the engine; they are separate because the two runtimes are
not combined.

```bash
# Bounded live-pipeline smoke runs under each sanitizer (Ctrl+C equivalent after 10 s)
timeout -s INT 10 ./build/blitz_lob_asan
timeout -s INT 10 ./build/blitz_lob_tsan
```

Running `./build/blitz_lob` with no flags starts the live synthetic pipeline
until Ctrl+C. It is a smoke test and prints no latency numbers.

Formatting and Python checks (versions are pinned in `requirements-dev.txt`):

```bash
python3 -m pip install -r requirements-dev.txt -r charts/requirements.txt
./scripts/format.sh --check          # clang-format 21.1.8 over tracked C/C++ files
./scripts/format.sh --write          # apply it
python3 -m black --check charts/generate_charts.py tests/test_chart_reporting.py
python3 -m unittest discover -s tests -p 'test_chart_reporting.py'
```

The Formatting GitHub workflow runs the two `--check` commands. CI does not
build or run the C++ tests.

---

## 2. Inspect the host

Read-only. Run these first to learn the real core numbers and whether tuning is
needed.

```bash
lscpu
nproc

# SMT sibling lists: a usable core is its own sole sibling
for c in /sys/devices/system/cpu/cpu*/topology/thread_siblings_list; do
    echo "$c: $(cat "$c")"
done

cat /sys/devices/system/cpu/isolated                         # should include 4-5 once tuned
cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor    # must be "performance"
cat /sys/devices/system/cpu/cpu5/cpufreq/scaling_governor
numactl --hardware                                           # which node owns cores 4/5
cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo "not intel_pstate"
cat /proc/sys/kernel/nmi_watchdog

ps -eLo pid,psr,pcpu,comm | awk '$2==4 || $2==5' | sort -k3 -nr | head -20
ps aux --sort=-%cpu | head -20
```

---

## 3. Reduce interference [ROOT]

```bash
# Close browsers and other CPU-heavy applications. Load on non-isolated cores
# can still inflate latency through the shared turbo and thermal budget.

sudo systemctl stop irqbalance          # otherwise it migrates IRQs onto isolated cores
sudo systemctl mask --runtime --now power-profiles-daemon   # D-Bus activated: stop alone is not enough; resets the governor
cat /proc/interrupts                    # inspect IRQ placement and adjust per host
# Move device IRQs off the pinned cores (a few hundred IRQs per run on core 5 inflate the tail).
# Some managed IRQs (NVMe) refuse the write; that is fine.
for f in /proc/irq/[0-9]*/smp_affinity_list; do
    echo 0-3,6-11 | sudo tee "$f" >/dev/null 2>&1
done
grep -H . /proc/irq/*/effective_affinity_list | grep -E ':(4|5)$'   # expect little or nothing
sudo systemctl stop cron atd unattended-upgrades 2>/dev/null   # adjust to what is running

ps -eLo pid,psr,pcpu,comm | awk '$2==4 || $2==5'   # should be empty or idle
```

---

## 4. Tune the CPU [ROOT]

```bash
# 1. Performance governor on both pinned cores (required by the preflight gate)
sudo cpupower -c 4,5 frequency-set --governor performance
# without cpupower:
echo performance | sudo tee /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor
echo performance | sudo tee /sys/devices/system/cpu/cpu5/cpufreq/scaling_governor

# 2. No SMT sibling on either pinned core is required. Disabling SMT globally
#    is optional when the chosen cores already have no sibling:
echo off | sudo tee /sys/devices/system/cpu/smt/control

# 3. Boot parameters. Add isolcpus=4,5 nohz_full=4,5 rcu_nocbs=4,5 to the kernel
#    command line without duplicating existing entries, then reboot.
sudoedit /etc/default/grub
sudo update-grub
sudo reboot

# 4. Optional: turbo off and NMI watchdog off (less jitter, lower peak clock)
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
echo 0 | sudo tee /proc/sys/kernel/nmi_watchdog
```

One-shot runtime version of the steps above (governor, IRQ affinity, stopping services):
`sudo scripts/tune_host.sh`. It resets on reboot.

Shared L2: on the 12th-gen hybrid CPU used here, cores 4-7 form one E-core cluster with a
shared L2 (`cat /sys/devices/system/cpu/cpu4/cache/index2/shared_cpu_list` prints `4-7`).
Work on cores 6 and 7 raised P99.9 by about 1.5x in a test, so for quiet measurements
isolate the whole cluster (`isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7`) and keep it idle.
The preflight does not check this.

What each setting does: `isolcpus` removes the cores from the scheduler pool,
`nohz_full` suppresses the periodic tick on them, `rcu_nocbs` moves RCU
callbacks off them. NUMA binding is applied per run with
`numactl --cpunodebind=0 --membind=0`; confirm node 0 is right from
`numactl --hardware`.

The core numbers are host-specific. On the 12th-gen Intel hybrid CPU used for
development, cores 2/3 are the two SMT threads of one P-core (disabling SMT
takes core 3 offline, which fails the SMT check), while cores 4+ are E-cores
with no sibling. Check `thread_siblings_list` and pick two cores that are each
their own sole sibling.

---

## 5. Verify tuning

```bash
cat /sys/devices/system/cpu/isolated                              # expect: 4-5
cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor         # expect: performance
cat /sys/devices/system/cpu/cpu5/cpufreq/scaling_governor         # expect: performance
cat /sys/devices/system/cpu/cpu4/topology/thread_siblings_list    # expect: 4
cat /sys/devices/system/cpu/cpu5/topology/thread_siblings_list    # expect: 5
ps -eLo pid,psr,pcpu,comm | awk '$2==4 || $2==5'                  # expect: empty or idle
```

The benchmark's preflight (`run_preflight_checks()` in `src/benchmark.cpp`)
enforces, for each pinned core, isolation membership, the `performance`
governor, and no SMT sibling, and refuses to measure otherwise. It does not
check `nohz_full`, `rcu_nocbs`, IRQ placement, NUMA binding, or other
processes, so confirm those yourself.

---

## 6. Datasets

`blitz_gen_dataset` flags other than `--output` default to the canonical values
from `config.hpp`, so the short form below equals the explicit one. Output is
deterministic for a given flag set. Flags are recorded in
`datasets/manifest.txt`.

```bash
# Canonical mixed workload: 110,000 events (10,000 warmup + 100,000 measured)
./build/blitz_gen_dataset --seed 42 --count 110000 --output datasets/bench_110k.bin

# Explicit equivalent
./build/blitz_gen_dataset --seed 42 --count 110000 --mid-price 100000 \
    --spread 500 --min-qty 1 --max-qty 1000 --cancel-ratio 0.15 \
    --ioc-ratio 0.05 --fok-ratio 0.02 --rate-hz 100000 \
    --output datasets/bench_110k.bin

# Flat-layout ablation dataset (its Order is 88 bytes, so the file layout differs;
# run_ablations.sh also does this automatically)
cmake --build build --target blitz_gen_dataset_ablation_flat_layout -j"$(nproc)"
./build/blitz_gen_dataset_ablation_flat_layout --seed 42 --count 110000 \
    --output datasets/ablation4_flat.bin
```

Other flags: `--replace-ratio F` (default 0) and `--client-id-count N` (default
8; a smaller value raises the self-trade rate).

---

## 7. Benchmarks

The benchmark pins itself to the configured cores, so prefix every run with
`numactl`. Use a clean tree for citable results; for deliberate experiments
with uncommitted changes, rebuild and add `--allow-dirty` (the output still
records the dirty state, and such runs must not be cited as measurements of a
clean commit). Runs below 5 trials are marked as smoketests.

```bash
commit="$(git rev-parse --short HEAD)"
mkdir -p "results/$commit"
```

### Canonical runs

```bash
numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob --benchmark --mode price_time --trials 5 \
    --dataset datasets/bench_110k.bin --output "results/$commit/price_time.csv"

numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob --benchmark --mode pro_rata --trials 5 \
    --dataset datasets/bench_110k.bin --output "results/$commit/pro_rata.csv"
```

Add `--verbose` for workload, pool, and queue diagnostics in the console
report. Each run writes `<name>.csv`, `<name>.raw.csv`, and `<name>.meta.json`
(see [`METHODOLOGY.md`](METHODOLOGY.md#output-files)).

### Ablation suite (both policies, baseline plus four variants, 10 runs)

```bash
./scripts/run_ablations.sh --trials 5            # add --allow-dirty for a dirty tree
# RESULTS_ROOT=results_new ./scripts/run_ablations.sh --trials 5   # write elsewhere than results/
```

The script builds the targets, regenerates the flat-layout dataset, runs
everything under `numactl --cpunodebind=0 --membind=0`, and writes
`results/<hash>/ablation_<variant>_<mode>.csv` plus
`ablation_summary.{csv,md}` with PROVEN, INCONCLUSIVE, NOT PROVEN, REGRESSION,
or INVALID verdicts. It requires `datasets/bench_110k.bin` to exist.

One variant by hand (build it first with
`cmake --build build --target blitz_lob_ablation_mutex_queue`):

```bash
numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob_ablation_mutex_queue --benchmark --mode price_time --trials 5 \
    --dataset datasets/bench_110k.bin --output /tmp/mutex_queue_pt.csv
```

Variants are `blitz_lob_ablation_{mutex_queue,unpinned,malloc_pool,flat_layout}`;
the flat-layout one takes `--dataset datasets/ablation4_flat.bin`.

### Smoketest and sanitizer benchmark runs (never citable)

```bash
numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob --benchmark --mode price_time --trials 1 \
    --dataset datasets/bench_110k.bin --output /tmp/smoketest.csv

# Benchmark path under each sanitizer, both policies
for mode in price_time pro_rata; do
    ./build/blitz_lob_asan --benchmark --mode "$mode" --trials 1 \
        --dataset datasets/bench_110k.bin --output "/tmp/asan_$mode.csv"
    ./build/blitz_lob_tsan --benchmark --mode "$mode" --trials 1 \
        --dataset datasets/bench_110k.bin --output "/tmp/tsan_$mode.csv"
done
```

---

## 8. Results and charts

```bash
ls "results/$commit/"

# Check that both canonical runs are valid and deterministic before citing them
python3 - "results/$commit" <<'PY'
import csv, sys
for name in ("price_time.csv", "pro_rata.csv"):
    rows = list(csv.DictReader(open(f"{sys.argv[1]}/{name}")))
    print(name,
          "valid=" + str(all(r["valid"] == "1" for r in rows)),
          "deterministic=" + str(all(r["determinism_status"] == "PASS" for r in rows)),
          "dropped=" + str(sum(int(r["dropped_events"]) for r in rows)))
PY

# Regenerate the README charts from a results directory (overwrites charts/*.png)
python3 charts/generate_charts.py --commit "$commit"
```

Chart generation overwrites the committed PNGs, which currently show the
`98a597b` evidence set; regenerate them only when publishing a new one.
