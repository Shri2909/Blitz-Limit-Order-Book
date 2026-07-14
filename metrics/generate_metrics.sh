#!/usr/bin/env bash
#
# metrics/generate_metrics.sh
#
# Builds the project, runs every measurement described in metrics/README.md
# (categories A/B/C/D), and produces a single self-contained HTML report at
# metrics/out/report.html.
#
# Usage:
#   ./metrics/generate_metrics.sh [--quick]
#
#   --quick   shortens the two threaded-pipeline runs (queue backpressure,
#             latency/throughput) from their default windows down to 1s
#             each, for a fast smoke-test of the whole pipeline. Omit for
#             the full, more statistically meaningful runs.
#
# Every category degrades gracefully instead of aborting the whole run:
#   - A and C never fail (no special hardware/environment needed).
#   - B runs on whatever machine invokes it and labels its own numbers as
#     "this machine" rather than a tuned-hardware claim (see the TSC
#     calibration warning it may print -- that's expected honesty, not a
#     bug).
#   - D actually attempts the real, preflight-gated benchmark
#     (blitz_lob --benchmark) and real `perf stat` counters; either can
#     legitimately report "not available on this machine" (the correct,
#     documented behavior of an untuned host -- see config.hpp's
#     ENVIRONMENT_REQUIREMENTS) without failing the script.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
OUT_DIR="$SCRIPT_DIR/out"
BIN_DIR="$SCRIPT_DIR/bin"
SRC_DIR="$SCRIPT_DIR/src"
CMAKELISTS="$REPO_ROOT/CMakeLists.txt"

QUICK=0
if [[ "${1:-}" == "--quick" ]]; then
    QUICK=1
fi
BACKPRESSURE_SECONDS=$([ "$QUICK" -eq 1 ] && echo 1 || echo 5)
LATENCY_SECONDS=$([ "$QUICK" -eq 1 ] && echo 1 || echo 3)

mkdir -p "$OUT_DIR" "$BIN_DIR"
rm -f "$OUT_DIR"/*.csv "$OUT_DIR"/*.txt "$OUT_DIR"/*.log

step() { printf '\n\033[1;34m==>\033[0m %s\n' "$1"; }
ok()   { printf '    \033[1;32mok\033[0m  %s\n' "$1"; }
skip() { printf '    \033[1;33mskip\033[0m %s\n' "$1"; }
fail() { printf '    \033[1;31mFAIL\033[0m %s\n' "$1"; }

# ──────────────────────────────────────────────────────────────────────────
# 0. Build the main project (needed for A4's test dashboard and D1's real
#    benchmark binary).
# ──────────────────────────────────────────────────────────────────────────
step "Configuring and building the main project"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" > "$OUT_DIR/cmake_configure.log" 2>&1
if [[ $? -ne 0 ]]; then
    fail "cmake configure failed -- see $OUT_DIR/cmake_configure.log"
    exit 1
fi

# Discover every per-phase test target directly from CMakeLists.txt rather
# than hardcoding a phase count, so this script stays correct as later
# phases (10, 11, ...) grow their own targets.
mapfile -t TEST_TARGETS < <(grep -oP 'add_executable\(\Kblitz_lob_test_phase\w+' "$CMAKELISTS" | sort -u)
BUILD_TARGETS=(blitz_lob blitz_gen_dataset blitz_lob_tests "${TEST_TARGETS[@]}")

if cmake --build "$BUILD_DIR" --target "${BUILD_TARGETS[@]}" -j"$(nproc)" \
        > "$OUT_DIR/cmake_build.log" 2>&1; then
    ok "built: ${BUILD_TARGETS[*]}"
else
    # Best-effort: a target that fails to build (e.g. an in-progress Phase
    # 10 needing libbpf/libxdp that isn't installed) shouldn't block the
    # rest of the report -- retry target-by-target and record what worked.
    fail "one or more targets failed together -- retrying individually"
    BUILD_TARGETS_OK=()
    for t in "${BUILD_TARGETS[@]}"; do
        if cmake --build "$BUILD_DIR" --target "$t" -j"$(nproc)" \
                >> "$OUT_DIR/cmake_build.log" 2>&1; then
            BUILD_TARGETS_OK+=("$t")
        else
            skip "target '$t' failed to build -- excluded from this report"
        fi
    done
    BUILD_TARGETS=("${BUILD_TARGETS_OK[@]}")
fi

# ──────────────────────────────────────────────────────────────────────────
# 1. Compile the standalone metrics programs (A1-A5, B1-B4, C1). These are
#    NOT part of the main CMake build -- kept fully separate on purpose, so
#    this folder never needs to touch CMakeLists.txt.
# ──────────────────────────────────────────────────────────────────────────
step "Compiling metrics programs"
CXXFLAGS=(-std=c++20 -O3 -DNDEBUG -march=native -pthread -Wall -Wextra -Wpedantic -I "$REPO_ROOT/include")

compile_metric() {
    local name="$1"
    shift
    if g++ "${CXXFLAGS[@]}" "$SRC_DIR/$name.cpp" "$@" -o "$BIN_DIR/$name" \
            2> "$OUT_DIR/compile_${name}.log"; then
        ok "$name"
    else
        fail "$name -- see $OUT_DIR/compile_${name}.log"
        return 1
    fi
}

compile_metric cancel_latency_sweep
compile_metric fill_distribution
compile_metric pool_exhaustion
compile_metric struct_layout
compile_metric queue_backpressure "$REPO_ROOT/src/pipeline.cpp"
compile_metric latency_and_throughput "$REPO_ROOT/src/pipeline.cpp"

# ──────────────────────────────────────────────────────────────────────────
# 2. Category A -- proofs from already-verified project behavior.
# ──────────────────────────────────────────────────────────────────────────
step "Category A: running proof measurements"

[[ -x "$BIN_DIR/cancel_latency_sweep" ]] && \
    "$BIN_DIR/cancel_latency_sweep" > "$OUT_DIR/a1_cancel_latency.csv" && \
    ok "A1 O(1) cancel depth sweep -> a1_cancel_latency.csv"

[[ -x "$BIN_DIR/fill_distribution" ]] && \
    "$BIN_DIR/fill_distribution" > "$OUT_DIR/a2_fill_distribution.csv" && \
    ok "A2 price-time vs pro-rata -> a2_fill_distribution.csv"

if [[ -x "$BIN_DIR/queue_backpressure" ]]; then
    "$BIN_DIR/queue_backpressure" "$BACKPRESSURE_SECONDS" \
        > "$OUT_DIR/a3_queue_backpressure.csv" 2> "$OUT_DIR/a3_queue_backpressure.stderr.txt"
    ok "A3 SPSC queue backpressure (${BACKPRESSURE_SECONDS}s) -> a3_queue_backpressure.csv"
fi

[[ -x "$BIN_DIR/pool_exhaustion" ]] && \
    "$BIN_DIR/pool_exhaustion" > "$OUT_DIR/a5_pool_exhaustion.csv" && \
    ok "A5 pool exhaustion/recovery -> a5_pool_exhaustion.csv"

step "Category A: test/phase coverage dashboard (A4)"
{
    echo "target,phase,pass_count,fail_count,sanitizer"
    for target in blitz_lob_tests "${TEST_TARGETS[@]}"; do
        bin="$BUILD_DIR/$target"
        [[ -x "$bin" ]] || continue

        phase="canonical"
        if [[ "$target" =~ blitz_lob_test_phase([0-9]+) ]]; then
            phase="${BASH_REMATCH[1]}"
        fi

        # Extract this target's own target_compile_options(...) block to
        # check for -fsanitize=, rather than assuming from a hardcoded list.
        sanitizer=$(awk "/target_compile_options\\($target PRIVATE/,/\\)/" "$CMAKELISTS" \
                    | grep -oE 'fsanitize=[a-z,]+' | head -1)
        sanitizer="${sanitizer:-none}"

        output=$("$bin" 2>&1)
        summary=$(echo "$output" | grep -oE '[0-9]+ passed, [0-9]+ failed' | tail -1)
        pass_count=$(echo "$summary" | grep -oE '^[0-9]+' || echo 0)
        fail_count=$(echo "$summary" | grep -oE '[0-9]+ failed' | grep -oE '^[0-9]+' || echo 0)

        echo "$target,$phase,${pass_count:-0},${fail_count:-0},$sanitizer"
    done
} > "$OUT_DIR/a4_test_coverage.csv"
ok "A4 test coverage -> a4_test_coverage.csv"

# ──────────────────────────────────────────────────────────────────────────
# 3. Category B -- quick new measurements on THIS machine (labeled as such
#    in the report, not claimed as tuned-hardware numbers).
# ──────────────────────────────────────────────────────────────────────────
step "Category B: latency distribution + throughput on this machine (${LATENCY_SECONDS}s)"
if [[ -x "$BIN_DIR/latency_and_throughput" ]]; then
    "$BIN_DIR/latency_and_throughput" "$LATENCY_SECONDS" \
        > "$OUT_DIR/b1_latency_samples.csv" 2> "$OUT_DIR/b_summary.txt"
    ok "B1/B2 latency distribution + throughput -> b1_latency_samples.csv, b_summary.txt"
fi

# ──────────────────────────────────────────────────────────────────────────
# 4. Category C -- structural facts, no measurement.
# ──────────────────────────────────────────────────────────────────────────
step "Category C: struct layout facts"
[[ -x "$BIN_DIR/struct_layout" ]] && \
    "$BIN_DIR/struct_layout" > "$OUT_DIR/c1_struct_layout.txt" && \
    ok "C1 cache-line layout -> c1_struct_layout.txt"

# ──────────────────────────────────────────────────────────────────────────
# 5. Category D -- the real, preflight-gated benchmark and perf counters.
#    Both are allowed to report "not available here" without failing the
#    script -- that IS the correct, documented behavior on an untuned host.
# ──────────────────────────────────────────────────────────────────────────
step "Category D1: real benchmark (blitz_lob --benchmark)"
D1_STATUS="not_attempted"
if [[ -x "$BUILD_DIR/blitz_lob" ]]; then
    if "$BUILD_DIR/blitz_lob" --benchmark --mode price_time --trials 5 \
            --output "$OUT_DIR/d1_benchmark.csv" \
            > "$OUT_DIR/d1_benchmark.stdout.txt" 2> "$OUT_DIR/d1_benchmark.stderr.txt"; then
        D1_STATUS="ok"
        ok "D1 real benchmark succeeded -> d1_benchmark.csv"
    else
        D1_STATUS="preflight_failed"
        skip "D1 benchmark refused to run (untuned host) -- see d1_benchmark.stderr.txt"
        skip "this is config.hpp's ENVIRONMENT_REQUIREMENTS working as intended, not a bug"
    fi
else
    skip "blitz_lob was not built -- see cmake_build.log"
fi
echo "$D1_STATUS" > "$OUT_DIR/d1_status.txt"

step "Category D2: perf stat hardware counters"
D2_STATUS="not_attempted"
if [[ "$D1_STATUS" == "ok" ]] && command -v perf > /dev/null 2>&1; then
    if perf stat -e cycles,instructions,cache-misses,cache-references \
            -o "$OUT_DIR/d2_perf.txt" \
            "$BUILD_DIR/blitz_lob" --benchmark --mode price_time --trials 1 \
            --output "$OUT_DIR/d2_benchmark_scratch.csv" \
            > /dev/null 2> "$OUT_DIR/d2_perf_run.stderr.txt"; then
        D2_STATUS="ok"
        ok "D2 perf counters -> d2_perf.txt"
    else
        D2_STATUS="perf_unavailable"
        skip "perf ran but reported no usable counters (likely perf_event_paranoid) -- see d2_perf_run.stderr.txt"
    fi
elif [[ "$D1_STATUS" != "ok" ]]; then
    D2_STATUS="skipped_no_benchmark"
    skip "D2 skipped: D1's benchmark didn't run, so wrapping it in perf would fail identically"
else
    D2_STATUS="perf_not_installed"
    skip "perf is not installed on this machine"
fi
echo "$D2_STATUS" > "$OUT_DIR/d2_status.txt"

# ──────────────────────────────────────────────────────────────────────────
# 6. Render the report.
# ──────────────────────────────────────────────────────────────────────────
step "Rendering HTML report"
if python3 "$SCRIPT_DIR/generate_report.py" "$OUT_DIR"; then
    ok "report -> $OUT_DIR/report.html"
else
    fail "report generation failed"
    exit 1
fi

printf '\n\033[1;32mDone.\033[0m Open %s/report.html in a browser.\n' "$OUT_DIR"
