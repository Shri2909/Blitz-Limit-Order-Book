# Blitz LOB — Complete Command Sequence: Zero-Interference Benchmarking

Run these phases **in order, top to bottom, on one machine, in one session**.
Each phase assumes the previous one succeeded. Commands marked **[ROOT]**
need `sudo`/root and are not run by the agent automatically in a sandboxed
session with no passwordless sudo — run them yourself, then continue.

This project hardcodes `RX_CORE_ID=4` / `MATCHING_CORE_ID=5`
(`include/hydra/config.hpp`) for **this specific host's** CPU topology
(12th-gen Intel hybrid: P-cores 0–3 have SMT siblings, E-cores 4+ do not —
see that file's own comment). **Re-verify core numbers with Phase 1 below
before reusing any command here on different hardware.**

---

## Phase 1 — Inspect current state (read-only, no root needed)

Run these first, every time — they tell you whether Phase 2/3 tuning is
even necessary, and give you the exact core numbers to use everywhere else.

```bash
# CPU model, core/thread counts, per-core topology
lscpu
nproc

# Which cores are hyperthread siblings of which (E-cores should show as
# their own singleton sibling list; P-cores show pairs)
for c in /sys/devices/system/cpu/cpu*/topology/thread_siblings_list; do
    echo "$c: $(cat "$c")"
done

# Currently isolated cores (should include 4,5 for this project once tuned)
cat /sys/devices/system/cpu/isolated

# Current governor per core (must be "performance" on cores 4,5)
cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor
cat /sys/devices/system/cpu/cpu5/cpufreq/scaling_governor

# NUMA topology (confirm which node cores 4,5 belong to -- used by
# --cpunodebind/--membind later)
numactl --hardware

# Turbo boost state (Intel P-state driver)
cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo "not intel_pstate"

# NMI watchdog (a real, if small, source of periodic interrupt jitter)
cat /proc/sys/kernel/nmi_watchdog

# What's actually running on cores 4/5 right now
ps -eLo pid,psr,pcpu,comm | awk '$2==4 || $2==5' | sort -k3 -nr | head -20

# Top CPU consumers system-wide (identify anything noisy before killing it)
ps aux --sort=-%cpu | head -20
```

---

## Phase 2 — Kill background interference **[ROOT for some]**

```bash
# Close any GUI browser/apps -- this project's own README documents a real,
# measured case where a background browser alone moved P99.9 from 1205ns to
# 7382ns. Close them by hand, or:
pkill -f firefox 2>/dev/null
pkill -f chrome 2>/dev/null
pkill -f chromium 2>/dev/null

# Stop irqbalance -- it will otherwise migrate IRQs onto your isolated
# cores over time. [ROOT]
sudo systemctl stop irqbalance
sudo systemctl disable irqbalance   # optional: keep it off across reboots

# Move existing IRQ affinity OFF cores 4 and 5 onto some other core (core 0
# here -- adjust if 0 is busy). Run once after the above. [ROOT]
for irq in /proc/irq/*/smp_affinity_list; do
    echo 0 | sudo tee "$irq" > /dev/null 2>&1
done

# Stop routine timers/services that can wake up and steal a cycle mid-run.
# Adjust this list to what's actually running on your box (see Phase 1's
# `ps aux` output) -- these are common, safe-to-stop-temporarily examples:
sudo systemctl stop cron atd unattended-upgrades 2>/dev/null

# Confirm nothing user-visible is left running on cores 4/5:
ps -eLo pid,psr,pcpu,comm | awk '$2==4 || $2==5'
```

---

## Phase 3 — CPU tuning **[ROOT]**

```bash
# 1. Performance governor on the two cores this project pins to (required
#    by run_benchmark()'s own preflight gate -- it will refuse to run
#    without this):
sudo cpupower frequency-set -c 4,5 --governor performance
# or, if cpupower isn't installed:
echo performance | sudo tee /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor
echo performance | sudo tee /sys/devices/system/cpu/cpu5/cpufreq/scaling_governor

# 2. SMT off (required by preflight; already structurally true for cores
#    4/5 on this host's hybrid topology -- confirmed in Phase 1 above -- but
#    if you're on different hardware where 4/5 DO have siblings, disable
#    SMT machine-wide):
echo off | sudo tee /sys/devices/system/cpu/smt/control

# 3. isolcpus / nohz_full / rcu_nocbs (required by preflight; needs a GRUB
#    edit + REBOOT -- do this once, it persists across reboots):
sudo sed -i 's/^GRUB_CMDLINE_LINUX="/GRUB_CMDLINE_LINUX="isolcpus=4,5 nohz_full=4,5 rcu_nocbs=4,5 /' /etc/default/grub
sudo update-grub
sudo reboot
# --- after reboot, re-run Phase 1's `cat /sys/devices/system/cpu/isolated`
#     to confirm it now reads "4-5" before continuing ---

# 4. Turbo boost OFF (optional, not required by this project's preflight,
#    but reduces P-state-transition jitter at the cost of raw clock speed --
#    HFT-style low-jitter tuning trades peak speed for consistency):
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo

# 5. NMI watchdog OFF (optional -- removes a small periodic interrupt
#    source from every core, including your isolated ones):
echo 0 | sudo tee /proc/sys/kernel/nmi_watchdog

# 6. NUMA binding is applied per-run, not persistently -- see Phase 6/7's
#    `numactl --cpunodebind=0 --membind=0` prefix on every benchmark
#    invocation. Confirm node 0 is correct for cores 4/5 from Phase 1's
#    `numactl --hardware` output before assuming it.
```

---

## Phase 4 — Verify tuning actually took effect (read-only)

Re-run Phase 1's inspection commands and confirm, explicitly, before
proceeding:

```bash
cat /sys/devices/system/cpu/isolated                              # expect: 4-5
cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor          # expect: performance
cat /sys/devices/system/cpu/cpu5/cpufreq/scaling_governor          # expect: performance
cat /sys/devices/system/cpu/cpu4/topology/thread_siblings_list     # expect: just "4"
cat /sys/devices/system/cpu/cpu5/topology/thread_siblings_list     # expect: just "5"
ps -eLo pid,psr,pcpu,comm | awk '$2==4 || $2==5'                   # expect: empty/idle
```

If any of these don't match, **stop and fix it now** — `run_benchmark()`'s
own preflight gate will refuse to run otherwise (by design), but a
partially-tuned host that happens to still pass preflight (e.g. governor
right but a stray process still on core 4) will silently produce worse
numbers without telling you why.

---

## Phase 5 — Clean build

```bash
cd /home/abc/projects/Blitz-Limit-Order-Book

# Fully clean (removes any stale object files from a different tuning state)
rm -rf build
cmake -S . -B build -DENABLE_AFXDP=OFF
cmake --build build -j"$(nproc)"
```

---

## Phase 6 — Correctness gate (must be green before any number counts)

```bash
# Full per-phase test suite
ctest --test-dir build --output-on-failure

# Dynamic sanitizer verification -- bounded live-pipeline smoke run
timeout -s INT 10 ./build/blitz_lob_asan
timeout -s INT 10 ./build/blitz_lob_tsan

# Sanitizer verification of the benchmark path specifically, BOTH matching
# policies (--trials 1 is a smoketest by this project's own convention --
# proves "clean," is never itself a citable latency number)
./build/blitz_lob_asan --benchmark --mode price_time --trials 1 \
    --dataset datasets/bench_110k.bin --output /tmp/asan_pt.csv
./build/blitz_lob_asan --benchmark --mode pro_rata --trials 1 \
    --dataset datasets/bench_110k.bin --output /tmp/asan_pr.csv
./build/blitz_lob_tsan --benchmark --mode price_time --trials 1 \
    --dataset datasets/bench_110k.bin --output /tmp/tsan_pt.csv
./build/blitz_lob_tsan --benchmark --mode pro_rata --trials 1 \
    --dataset datasets/bench_110k.bin --output /tmp/tsan_pr.csv
```

All of the above must be 100% pass / zero sanitizer reports before Phase 7.

---

## Phase 7 — Regenerate datasets (only if missing, or after any dataset-format change)

```bash
# Canonical mixed workload (110,000 events: 10k warmup + 100k measured)
./build/blitz_gen_dataset --seed 42 --count 110000 --mid-price 100000 \
    --spread 500 --min-qty 1 --max-qty 1000 --cancel-ratio 0.15 \
    --ioc-ratio 0.05 --fok-ratio 0.02 --rate-hz 100000 \
    --output datasets/bench_110k.bin

# Same logical dataset, re-serialized for the flat_layout ablation (its
# Order/DatasetRecord on-disk size differs -- see that target's own build)
cmake --build build --target blitz_gen_dataset_ablation_flat_layout -j"$(nproc)"
./build/blitz_gen_dataset_ablation_flat_layout --seed 42 --count 110000 \
    --mid-price 100000 --spread 500 --min-qty 1 --max-qty 1000 \
    --cancel-ratio 0.15 --ioc-ratio 0.05 --fok-ratio 0.02 --rate-hz 100000 \
    --output datasets/ablation4_flat.bin
```

---

## Phase 8 — The actual benchmarks (every type this system supports)

Every command below is prefixed with `numactl --cpunodebind=0 --membind=0`
(confirm node 0 is correct for your machine from Phase 1's
`numactl --hardware`). Commit the tree first if you want the `Commit`
field in every report to be trustworthy (drop `--allow-dirty` once you do):

```bash
git add -A && git commit -m "your message"
```

If you're iterating without committing yet, keep `--allow-dirty` on every
command below (it makes the run **allowed**, not silently treated as
clean — every report still marks it dirty).

### 8.1 — Canonical Price-Time (the "how fast is Price-Time" number)

```bash
mkdir -p results/$(git rev-parse --short HEAD)
numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob --benchmark --mode price_time --trials 5 \
    --dataset datasets/bench_110k.bin \
    --output results/$(git rev-parse --short HEAD)/price_time.csv \
    --allow-dirty
```

### 8.2 — Canonical Pro-Rata (the "how fast is Pro-Rata" number)

```bash
numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob --benchmark --mode pro_rata --trials 5 \
    --dataset datasets/bench_110k.bin \
    --output results/$(git rev-parse --short HEAD)/pro_rata.csv \
    --allow-dirty
```

### 8.3 — Verbose variants (adds per-workload/pool/queue diagnostics to the console report; same CSVs)

```bash
numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob --benchmark --mode price_time --trials 5 \
    --dataset datasets/bench_110k.bin \
    --output results/$(git rev-parse --short HEAD)/price_time_verbose.csv \
    --allow-dirty --verbose

numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob --benchmark --mode pro_rata --trials 5 \
    --dataset datasets/bench_110k.bin \
    --output results/$(git rev-parse --short HEAD)/pro_rata_verbose.csv \
    --allow-dirty --verbose
```

### 8.4 — Full optimization ablation suite (both policies, 5 variants each = 10 runs)

```bash
./scripts/run_ablations.sh --trials 5 --allow-dirty
```

Produces, per matching policy: `optimized`, `mutex_queue`, `unpinned`,
`malloc_pool`, `flat_layout` -- each a real, independently gated run, plus
`ablation_summary.{csv,md}` with PROVEN/INCONCLUSIVE/NOT PROVEN/REGRESSION/
INVALID verdicts (see `docs/BENCHMARK_METHODOLOGY.md` for exactly how each
verdict is computed).

### 8.5 — Individual ablation variants by hand (if you want to re-run just one, not the whole suite)

```bash
numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob_ablation_mutex_queue --benchmark --mode price_time --trials 5 \
    --dataset datasets/bench_110k.bin --output /tmp/mutex_queue_pt.csv --allow-dirty

numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob_ablation_unpinned --benchmark --mode price_time --trials 5 \
    --dataset datasets/bench_110k.bin --output /tmp/unpinned_pt.csv --allow-dirty

numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob_ablation_malloc_pool --benchmark --mode price_time --trials 5 \
    --dataset datasets/bench_110k.bin --output /tmp/malloc_pool_pt.csv --allow-dirty

numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob_ablation_flat_layout --benchmark --mode price_time --trials 5 \
    --dataset datasets/ablation4_flat.bin --output /tmp/flat_layout_pt.csv --allow-dirty
```

(Swap `--mode price_time` for `--mode pro_rata` on any of the above to get
that variant's Pro-Rata number instead.)

### 8.6 — Smoketest (fast iteration only -- NEVER a citable result)

```bash
numactl --cpunodebind=0 --membind=0 -- \
    ./build/blitz_lob --benchmark --mode price_time --trials 1 \
    --dataset datasets/bench_110k.bin --output /tmp/smoketest.csv --allow-dirty
```

The console report and CSV both mark any `--trials` below 5 as
`[SMOKETEST]` -- use this only to sanity-check a change quickly, never to
report a number.

---

## Phase 9 — Read the results

```bash
# Console output from each run above already prints the full report.
# The three artifacts per run:
ls results/$(git rev-parse --short HEAD)/

# price_time.csv / pro_rata.csv       -- summary, one row per trial
# price_time.raw.csv / pro_rata.raw.csv -- one row per sample
# price_time.meta.json / pro_rata.meta.json -- environment + provenance

# Confirm both canonical runs are VALID and deterministic before citing
# anything from them:
grep -m1 . results/$(git rev-parse --short HEAD)/price_time.csv | true
python3 -c "
import csv
for f in ['price_time.csv', 'pro_rata.csv']:
    path = 'results/$(git rev-parse --short HEAD)/' + f
    rows = list(csv.DictReader(open(path)))
    valid = all(r['valid'] == '1' for r in rows)
    determ = all(r['determinism_status'] == 'PASS' for r in rows)
    dropped = sum(int(r['dropped_events']) for r in rows)
    print(f, 'valid=' + str(valid), 'deterministic=' + str(determ), 'dropped=' + str(dropped))
"
```

---

## Quick reference: full sequence, no explanations

```bash
# --- one-time tuning (ROOT) ---
sudo systemctl stop irqbalance cron atd unattended-upgrades
sudo cpupower frequency-set -c 4,5 --governor performance
echo off | sudo tee /sys/devices/system/cpu/smt/control
sudo sed -i 's/^GRUB_CMDLINE_LINUX="/GRUB_CMDLINE_LINUX="isolcpus=4,5 nohz_full=4,5 rcu_nocbs=4,5 /' /etc/default/grub
sudo update-grub && sudo reboot
# (after reboot)
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo
echo 0 | sudo tee /proc/sys/kernel/nmi_watchdog

# --- verify ---
cat /sys/devices/system/cpu/isolated
cat /sys/devices/system/cpu/cpu{4,5}/cpufreq/scaling_governor

# --- build + test ---
cd /home/abc/projects/Blitz-Limit-Order-Book
rm -rf build && cmake -S . -B build -DENABLE_AFXDP=OFF && cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

# --- datasets ---
./build/blitz_gen_dataset --seed 42 --count 110000 --output datasets/bench_110k.bin
cmake --build build --target blitz_gen_dataset_ablation_flat_layout -j"$(nproc)"
./build/blitz_gen_dataset_ablation_flat_layout --seed 42 --count 110000 --output datasets/ablation4_flat.bin

# --- benchmarks ---
GIT=$(git rev-parse --short HEAD); mkdir -p "results/$GIT"
numactl --cpunodebind=0 --membind=0 -- ./build/blitz_lob --benchmark --mode price_time --trials 5 --dataset datasets/bench_110k.bin --output "results/$GIT/price_time.csv" --allow-dirty
numactl --cpunodebind=0 --membind=0 -- ./build/blitz_lob --benchmark --mode pro_rata  --trials 5 --dataset datasets/bench_110k.bin --output "results/$GIT/pro_rata.csv"  --allow-dirty
./scripts/run_ablations.sh --trials 5 --allow-dirty
```
