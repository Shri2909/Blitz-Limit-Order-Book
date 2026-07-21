#!/usr/bin/env bash
#
# scripts/run_ablations.sh
#
# Automates the four docs/DESIGN.md ablations (lock-free vs. mutex queue,
# pinned vs. unpinned, pooled vs. malloc, hot/cold vs. flat layout) for
# BOTH matching policies (price_time and pro_rata) -- 10 runs total, never
# just 5. This never mutates the real source tree: each ablation lives in
# its own directory under ablation/ and is built as a separate,
# EXCLUDE_FROM_ALL CMake target that shadows exactly one real header via
# include-path ordering (see ablation/mutex_queue/include/hydra/
# spsc_queue.hpp's own WHY for the mechanism).
#
# Usage:
#   ./scripts/run_ablations.sh [--trials N]
#
# Requires the same environment preflight --benchmark itself requires
# (isolcpus, performance governor, SMT off) -- every run below goes through
# the real, gated run_benchmark(), nothing here bypasses that.
#
# Reads each run's SUMMARY CSV (one row per trial, real named columns --
# see write_summary_csv() in benchmark.cpp) directly, not human-readable
# stdout text and not '#' comment lines (the old CSV shape's contract,
# which no longer exists). Cross-checks dropped_events, arena_fallback,
# and pool_exhaustion between baseline and every ablated variant BEFORE
# accepting a delta -- refuses to call a delta clean if either side saw any
# of these (closing the gap the prior version of this script left open).

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
GIT_HASH="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
OUT_DIR="$REPO_ROOT/results/$GIT_HASH"
DATASET="$REPO_ROOT/datasets/bench_110k.bin"
FLAT_DATASET="$REPO_ROOT/datasets/ablation4_flat.bin"

TRIALS=5
ALLOW_DIRTY=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --trials) TRIALS="$2"; shift 2 ;;
        --allow-dirty) ALLOW_DIRTY=1; shift 1 ;;
        *) echo "unknown flag: $1" >&2; exit 1 ;;
    esac
done

mkdir -p "$OUT_DIR"

if [[ ! -f "$DATASET" ]]; then
    echo "error: $DATASET does not exist -- generate the canonical dataset first:" >&2
    echo "  cmake --build build --target blitz_gen_dataset" >&2
    echo "  ./build/blitz_gen_dataset --seed 42 --count 110000 --output $DATASET" >&2
    exit 1
fi

DIRTY_FLAG=""
if [[ "$ALLOW_DIRTY" -eq 1 ]]; then
    DIRTY_FLAG="--allow-dirty"
fi

RAW_JSONL="$OUT_DIR/ablation_raw.jsonl"
: > "$RAW_JSONL"

# Runs one (label, binary, dataset, mode) combination, then extracts every
# per-trial row from its summary CSV into one JSON line appended to
# $RAW_JSONL -- the embedded Python block at the bottom of this script is
# the only place that then interprets those numbers into a verdict table.
run_one() {
    local label="$1" binary="$2" dataset="$3" mode="$4"
    local csv="$OUT_DIR/ablation_${label}_${mode}.csv"
    echo "=== $label ($mode): running --benchmark (${TRIALS} trials) ==="
    numactl --cpunodebind=0 --membind=0 -- \
        "$binary" --benchmark --mode "$mode" --trials "$TRIALS" \
        --dataset "$dataset" --output "$csv" $DIRTY_FLAG
    local rc=$?
    python3 - "$label" "$mode" "$csv" "$rc" >> "$RAW_JSONL" <<'PYEOF'
import csv, json, sys

label, mode, csv_path, rc = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])

if rc != 0:
    print(json.dumps({"label": label, "mode": mode, "ok": False, "reason": f"exit code {rc}"}))
    sys.exit(0)

try:
    with open(csv_path, newline="") as f:
        rows = list(csv.DictReader(f))
except OSError as e:
    print(json.dumps({"label": label, "mode": mode, "ok": False, "reason": str(e)}))
    sys.exit(0)

if not rows:
    print(json.dumps({"label": label, "mode": mode, "ok": False, "reason": "empty CSV"}))
    sys.exit(0)

p99s = [float(r["p99_ns_per_order"]) for r in rows]
p999s = [float(r["p999_ns_per_order"]) for r in rows]
throughputs = [float(r["throughput_orders_per_second"]) for r in rows]
dropped = sum(int(r["dropped_events"]) for r in rows)
arena = sum(int(r["arena_fallback_count"]) for r in rows)
pool = sum(int(r["pool_exhaustion_count"]) for r in rows)
valid = all(r["valid"] == "1" for r in rows)
determinism_ok = all(r["determinism_status"] == "PASS" for r in rows)

p99s.sort()
p999s.sort()
n = len(p99s)
median_p99 = p99s[n // 2] if n % 2 else (p99s[n // 2 - 1] + p99s[n // 2]) / 2.0
median_p999 = p999s[n // 2] if n % 2 else (p999s[n // 2 - 1] + p999s[n // 2]) / 2.0

mean_p99 = sum(p99s) / n
var_p99 = sum((x - mean_p99) ** 2 for x in p99s) / (n - 1) if n > 1 else 0.0
cv_p99 = (var_p99 ** 0.5) / mean_p99 if mean_p99 else 0.0

print(json.dumps({
    "label": label, "mode": mode, "ok": True,
    "median_p99_ns": median_p99, "median_p999_ns": median_p999,
    "cv_p99": cv_p99, "dropped_events": dropped, "arena_fallback_count": arena,
    "pool_exhaustion_count": pool, "valid": valid, "determinism_ok": determinism_ok,
    "trials": n,
}))
PYEOF
}

echo "=== building baseline + ablation targets ==="
cmake --build "$BUILD_DIR" --target blitz_lob \
    blitz_lob_ablation_mutex_queue blitz_lob_ablation_unpinned \
    blitz_lob_ablation_malloc_pool blitz_lob_ablation_flat_layout \
    blitz_gen_dataset_ablation_flat_layout -j"$(nproc)"

echo "=== flat_layout: generating ablation-compatible dataset ==="
"$BUILD_DIR/blitz_gen_dataset_ablation_flat_layout" --seed 42 --count 110000 \
    --mid-price 100000 --spread 500 --min-qty 1 --max-qty 1000 --cancel-ratio 0.15 \
    --ioc-ratio 0.05 --fok-ratio 0.02 --rate-hz 100000 \
    --output "$FLAT_DATASET" > /dev/null

for MODE in price_time pro_rata; do
    run_one "baseline"    "$BUILD_DIR/blitz_lob"                      "$DATASET"      "$MODE"
    run_one "mutex_queue" "$BUILD_DIR/blitz_lob_ablation_mutex_queue" "$DATASET"      "$MODE"
    run_one "unpinned"    "$BUILD_DIR/blitz_lob_ablation_unpinned"    "$DATASET"      "$MODE"
    run_one "malloc_pool" "$BUILD_DIR/blitz_lob_ablation_malloc_pool" "$DATASET"      "$MODE"
    run_one "flat_layout" "$BUILD_DIR/blitz_lob_ablation_flat_layout" "$FLAT_DATASET" "$MODE"
done

SUMMARY_CSV="$OUT_DIR/ablation_summary.csv"
SUMMARY_MD="$OUT_DIR/ablation_summary.md"

python3 - "$RAW_JSONL" "$SUMMARY_CSV" "$SUMMARY_MD" <<'PYEOF'
import csv, json, sys

raw_path, csv_path, md_path = sys.argv[1:4]
rows = {}
with open(raw_path) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        r = json.loads(line)
        rows[(r["label"], r["mode"])] = r

pairs = [
    ("mutex_queue", "Lock-free SPSC queue", "std::mutex + std::queue"),
    ("unpinned", "Core pinning + isolation", "Unpinned OS-scheduled threads"),
    ("malloc_pool", "Fixed-slab object pooling", "new/delete per order"),
    ("flat_layout", "Cache-line hot/cold split", "Flat, unseparated Order layout"),
]

# Mirrors hydra::stats::classify_ablation() exactly (include/hydra/stats.hpp)
# -- kept as an independent Python re-implementation here since this script
# runs before/without needing to link the C++ binary; if the two ever
# disagree, that's a bug worth finding, not a reason to only trust one.
def classify(baseline, ablated):
    if baseline is None or ablated is None:
        return "INVALID"
    if not baseline.get("ok") or not ablated.get("ok"):
        return "INVALID"
    if not baseline["valid"] or not ablated["valid"]:
        return "INVALID"
    if not baseline["determinism_ok"] or not ablated["determinism_ok"]:
        return "INVALID"
    if baseline["dropped_events"] != 0 or ablated["dropped_events"] != 0:
        return "INVALID"
    if baseline["arena_fallback_count"] != 0 or ablated["arena_fallback_count"] != 0:
        return "INVALID"
    if baseline["pool_exhaustion_count"] != 0 or ablated["pool_exhaustion_count"] != 0:
        return "INVALID"

    delta_p99 = ablated["median_p99_ns"] - baseline["median_p99_ns"]
    delta_p999 = ablated["median_p999_ns"] - baseline["median_p999_ns"]

    if delta_p99 < 0 and delta_p999 < 0:
        return "REGRESSION"

    noise_floor_cv = max(baseline["cv_p99"], ablated["cv_p99"])
    noise_floor_ns = noise_floor_cv * baseline["median_p99_ns"]

    if delta_p99 > noise_floor_ns and delta_p999 > 0:
        return "PROVEN"
    if delta_p99 <= 0 and delta_p999 <= 0:
        return "NOT PROVEN"
    return "INCONCLUSIVE"


all_csv_rows = []
all_md_sections = []

for mode in ("price_time", "pro_rata"):
    baseline = rows.get(("baseline", mode))
    md_lines = [
        f"\n### Matching policy: {mode}\n",
        "| Design decision | Baseline P99.9 (median) | Rejected alternative | P99.9 delta | Verdict |",
        "|---|---|---|---|---|",
    ]
    for key, decision, alt_name in pairs:
        row = rows.get((key, mode))
        verdict = classify(baseline, row)
        if baseline and baseline.get("ok") and row and row.get("ok"):
            b999 = baseline["median_p999_ns"]
            a999 = row["median_p999_ns"]
            delta_x = (a999 / b999) if b999 else 0.0
            all_csv_rows.append([mode, decision, f"{baseline['median_p99_ns']:.1f}",
                                  f"{b999:.1f}", alt_name, f"{row['median_p99_ns']:.1f}",
                                  f"{a999:.1f}", f"{delta_x:.2f}", baseline["dropped_events"],
                                  row["dropped_events"], verdict])
            md_lines.append(f"| {decision} | **{b999:.1f} ns** | {alt_name}: {a999:.1f} ns | "
                             f"**~{delta_x:.2f}x worse** | {verdict} |")
        else:
            reason = (row or {}).get("reason", "missing/failed run")
            all_csv_rows.append([mode, decision, "", "", alt_name, "", "", "", "", "", "INVALID"])
            md_lines.append(f"| {decision} | n/a | {alt_name}: FAILED ({reason}) | n/a | INVALID |")
    all_md_sections.append("\n".join(md_lines))

with open(csv_path, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["matching_mode", "design_decision", "baseline_p99_ns", "baseline_p99_9_ns",
                "ablated_label", "ablated_p99_ns", "ablated_p99_9_ns", "p99_9_delta_x",
                "dropped_events_baseline", "dropped_events_ablated", "verdict"])
    w.writerows(all_csv_rows)

with open(md_path, "w") as f:
    f.write("# Optimization Ablation Study\n" + "\n".join(all_md_sections) + "\n")

print("\n".join(all_md_sections))
PYEOF

echo
echo "=== ablation suite complete ==="
echo "raw results (jsonl): $RAW_JSONL"
echo "summary CSV:         $SUMMARY_CSV"
echo "summary (md):        $SUMMARY_MD"
echo "per-run CSVs:        $OUT_DIR/ablation_<label>_<mode>.csv (+ .raw.csv, .meta.json)"
echo
echo "Compare against a prior run's summary -- expect the same DIRECTION"
echo "(every ablation slower than baseline on both policies), not necessarily"
echo "the same multiplier (different machine/session/load)."
