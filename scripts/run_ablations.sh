#!/usr/bin/env bash
# Benchmark the baseline and four ablations under both matching policies (10 runs)
# and summarize each ablation against the baseline.
# Usage: [RESULTS_ROOT=dir] ./scripts/run_ablations.sh [--trials N] [--allow-dirty]
# Output goes to $RESULTS_ROOT/<commit> (default: results/<commit>).

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
GIT_HASH="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
OUT_DIR="${RESULTS_ROOT:-$REPO_ROOT/results}/$GIT_HASH"
DATASET="$REPO_ROOT/datasets/bench_110k.bin"
FLAT_DATASET="$OUT_DIR/ablation4_flat.bin"

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

# A failed benchmark is recorded as an invalid run; the suite keeps going.
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

p999_trials = list(p999s)  # per-trial P99.9 in trial order, input of the verdict rule
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
    "trials": n, "p999_trials": p999_trials,
}))
PYEOF
}

echo "=== building baseline + ablation targets ==="
if ! cmake --build "$BUILD_DIR" --target blitz_lob \
    blitz_lob_ablation_mutex_queue blitz_lob_ablation_unpinned \
    blitz_lob_ablation_malloc_pool blitz_lob_ablation_flat_layout \
    blitz_gen_dataset_ablation_flat_layout -j"$(nproc)"; then
    echo "error: ablation build failed; no benchmarks were run" >&2
    exit 1
fi

echo "=== flat_layout: generating ablation-compatible dataset ==="
if ! "$BUILD_DIR/blitz_gen_dataset_ablation_flat_layout" --seed 42 --count 110000 \
    --mid-price 100000 --spread 500 --min-qty 1 --max-qty 1000 --cancel-ratio 0.15 \
    --ioc-ratio 0.05 --fok-ratio 0.02 --rate-hz 100000 \
    --output "$FLAT_DATASET" > /dev/null; then
    echo "error: flat-layout dataset generation failed; no benchmarks were run" >&2
    exit 1
fi

for MODE in price_time pro_rata; do
    run_one "baseline"    "$BUILD_DIR/blitz_lob"                      "$DATASET"      "$MODE"
    run_one "mutex_queue" "$BUILD_DIR/blitz_lob_ablation_mutex_queue" "$DATASET"      "$MODE"
    run_one "unpinned"    "$BUILD_DIR/blitz_lob_ablation_unpinned"    "$DATASET"      "$MODE"
    run_one "malloc_pool" "$BUILD_DIR/blitz_lob_ablation_malloc_pool" "$DATASET"      "$MODE"
    run_one "flat_layout" "$BUILD_DIR/blitz_lob_ablation_flat_layout" "$FLAT_DATASET" "$MODE"
done

SUMMARY_CSV="$OUT_DIR/ablation_summary.csv"
SUMMARY_MD="$OUT_DIR/ablation_summary.md"

python3 - "$RAW_JSONL" "$SUMMARY_CSV" "$SUMMARY_MD" "$SCRIPT_DIR" <<'PYEOF'
import csv, json, sys

raw_path, csv_path, md_path, script_dir = sys.argv[1:5]
sys.path.insert(0, script_dir)
# Verdict rule shared with tests/test_verdict_parity.py; mirrors hydra::stats::classify_ablation().
from ablation_verdict import RULE_SUMMARY, METRIC, classify, verdict_text
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

all_csv_rows = []
all_md_sections = []

for mode in ("price_time", "pro_rata"):
    baseline = rows.get(("baseline", mode))
    md_lines = [
        f"\n### Matching policy: {mode}\n",
        "| Design decision | Baseline P99.9 (median) | Rejected alternative | P99.9 ratio (ablated / baseline) | Verdict |",
        "|---|---|---|---|---|",
    ]
    for key, decision, alt_name in pairs:
        row = rows.get((key, mode))
        code = classify(baseline, row)
        verdict = verdict_text(code)
        if baseline and baseline.get("ok") and row and row.get("ok"):
            b999 = baseline["median_p999_ns"]
            a999 = row["median_p999_ns"]
            delta_x = (a999 / b999) if b999 else 0.0
            all_csv_rows.append([mode, decision, f"{baseline['median_p99_ns']:.1f}",
                                  f"{b999:.1f}", alt_name, f"{row['median_p99_ns']:.1f}",
                                  f"{a999:.1f}", f"{delta_x:.2f}", baseline["dropped_events"],
                                  row["dropped_events"], code, METRIC if code != "INVALID" else ""])
            md_lines.append(f"| {decision} | **{b999:.1f} ns** | {alt_name}: {a999:.1f} ns | "
                             f"**{delta_x:.2f}x** | {verdict} |")
        else:
            reason = (row or {}).get("reason", "missing/failed run")
            all_csv_rows.append([mode, decision, "", "", alt_name, "", "", "", "", "", "INVALID", ""])
            md_lines.append(f"| {decision} | n/a | {alt_name}: FAILED ({reason}) | n/a | INVALID |")
    md_lines.append(f"\n*Verdict rule ({METRIC} per trial): {RULE_SUMMARY}*")
    all_md_sections.append("\n".join(md_lines))

with open(csv_path, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["matching_mode", "design_decision", "baseline_p99_ns", "baseline_p99_9_ns",
                "ablated_label", "ablated_p99_ns", "ablated_p99_9_ns", "p99_9_delta_x",
                "dropped_events_baseline", "dropped_events_ablated", "verdict", "verdict_metric"])
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
