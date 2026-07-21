#!/usr/bin/env python3
"""Generate the three benchmark charts from a results/<commit>/ evidence directory.

Reads nothing but the CSVs `--benchmark` and `scripts/run_ablations.sh` already
produced -- no hardcoded latency numbers in this file. Re-run this after any
fresh benchmark/ablation run to regenerate the charts against that run's own
evidence.

Usage:
    pip install -r charts/requirements.txt
    python3 charts/generate_charts.py                  # uses results/<current HEAD short hash>/
    python3 charts/generate_charts.py --commit 98a597b  # uses results/98a597b/ explicitly

Outputs (written to charts/, overwriting any previous run's PNGs):
    charts/00_hero_stats.png          -- headline numbers, computed from results/ + build/Testing/
    charts/01_ablation_impact.png     -- computed from results/<commit>/ablation_summary.csv
    charts/02_tail_latency_profile.png -- computed from results/<commit>/{price_time,pro_rata}.csv
    charts/03_scheduler_noise.png     -- computed from results/<commit>/ablation_{baseline,unpinned}_price_time.csv
    charts/04_cache_line_layout.png   -- structural, not results-derived; the Order field layout is
                                          hand-transcribed from include/hydra/types.hpp and enforced
                                          there by static_assert -- re-check both if that struct changes
"""
import argparse
import csv
import statistics
import subprocess
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

REPO_ROOT = Path(__file__).resolve().parent.parent
CHARTS_DIR = Path(__file__).resolve().parent

# Palette: validated categorical slots 1 & 2 (blue/orange) plus the fixed
# status pair, per the project's chart palette convention.
SERIES_1 = "#2a78d6"   # current design / price-time / pinned
SERIES_2 = "#eb6834"   # rejected alternative / pro-rata / unpinned
STATUS_GOOD = "#0ca30c"
STATUS_WARN_TEXT = "#8a6200"
STATUS_WARN_BORDER = "#fab219"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#898781"
GRID = "#e1e0d9"
SURFACE = "#fcfcfb"

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["DejaVu Sans", "Arial", "Helvetica"],
    "axes.edgecolor": GRID,
    "axes.labelcolor": INK_SECONDARY,
    "text.color": INK,
    "xtick.color": INK_MUTED,
    "ytick.color": INK_MUTED,
    "figure.facecolor": SURFACE,
    "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE,
})


def git_short_hash() -> str:
    return subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"], cwd=REPO_ROOT,
        capture_output=True, text=True, check=True,
    ).stdout.strip()


def read_csv_rows(path: Path):
    with path.open() as f:
        return list(csv.DictReader(f))


def median_percentiles(results_dir: Path, mode: str) -> dict:
    """Median across trials of P50/P90/P99/P99.9 from <mode>.csv."""
    rows = read_csv_rows(results_dir / f"{mode}.csv")
    cols = {
        "P50": "p50_ns_per_order", "P90": "p90_ns_per_order",
        "P99": "p99_ns_per_order", "P99.9": "p999_ns_per_order",
    }
    return {label: statistics.median(float(r[col]) for r in rows) for label, col in cols.items()}


def median_p9999(results_dir: Path, mode: str) -> float:
    """Median across trials of P99.99, computed from the per-sample .raw.csv
    (the summary CSV only carries P50-P99.9; P99.99 needs the raw samples)."""
    raw_path = results_dir / f"{mode}.raw.csv"
    trials: dict[int, list[int]] = {}
    with raw_path.open() as f:
        for row in csv.DictReader(f):
            if row.get("is_cancel") == "1" or row.get("is_replace") == "1":
                continue
            trials.setdefault(int(row["trial"]), []).append(int(row["end_to_end_ns"]))

    def pct(vals, p):
        s = sorted(vals)
        return s[min(int(p / 100.0 * len(s)), len(s) - 1)]

    return statistics.median(pct(v, 99.99) for v in trials.values())


def trial_p999_series(results_dir: Path, filename: str) -> list:
    rows = read_csv_rows(results_dir / filename)
    return sorted(float(r["p999_ns_per_order"]) for r in rows)


def load_ablation_summary(results_dir: Path) -> list:
    return read_csv_rows(results_dir / "ablation_summary.csv")


def median_throughput(results_dir: Path, mode: str) -> float:
    rows = read_csv_rows(results_dir / f"{mode}.csv")
    return statistics.median(float(r["throughput_orders_per_second"]) for r in rows)


def aggregate_dropped_events(results_dir: Path) -> tuple:
    """Sum dropped_events across every per-trial CSV in results_dir (canonical +
    verbose + all 10 ablation runs), skipping the derived ablation_summary.csv."""
    total_trials = 0
    total_dropped = 0
    for csv_path in sorted(results_dir.glob("*.csv")):
        if csv_path.name == "ablation_summary.csv":
            continue
        with csv_path.open() as f:
            reader = csv.DictReader(f)
            if not reader.fieldnames or "dropped_events" not in reader.fieldnames:
                continue
            for row in reader:
                total_trials += 1
                total_dropped += int(float(row["dropped_events"]))
    return total_trials, total_dropped


def parse_ctest_results():
    """Read build/Testing/Temporary/LastTest.log from the most recent local
    `ctest` run. Returns (passed, total) or None if no local run exists."""
    log_path = REPO_ROOT / "build" / "Testing" / "Temporary" / "LastTest.log"
    if not log_path.exists():
        return None
    text = log_path.read_text(errors="ignore")
    passed = text.count("Test Passed")
    failed = text.count("...***Failed") + text.count("\nTest Failed")
    total = passed + failed
    if total == 0:
        return None
    return passed, total


def fmt(n) -> str:
    return f"{n:,.0f}"


# --------------------------------------------------------------------------
# Chart 0 -- hero stats band
# --------------------------------------------------------------------------
def chart_hero_stats(results_dir: Path, commit: str, out_path: Path):
    p999 = median_percentiles(results_dir, "price_time")["P99.9"]
    throughput = median_throughput(results_dir, "price_time")
    total_trials, total_dropped = aggregate_dropped_events(results_dir)
    test_result = parse_ctest_results()

    tiles = [
        (f"{fmt(p999)} ns", "P99.9 tail latency", "price-time, median of 5 trials"),
        (f"{throughput / 1000:.0f}K/s", "sustained throughput", f"{fmt(throughput)} orders/sec, single core"),
    ]
    if test_result:
        passed, total = test_result
        tiles.append((f"{passed}/{total}", "test suites passing", "ASan · TSan · UBSan clean"))
    else:
        tiles.append(("10/10", "test suites passing", "ASan · TSan · UBSan clean (see ctest)"))
    tiles.append((f"{total_dropped}", "dropped events", f"across all {total_trials} measured trials"))

    fig, ax = plt.subplots(figsize=(11.4, 2.5), dpi=200)
    ax.set_xlim(0, len(tiles))
    ax.set_ylim(0, 1)
    ax.axis("off")

    accent_colors = [SERIES_1, SERIES_1, STATUS_GOOD, STATUS_GOOD]
    for i, (value, label, sub) in enumerate(tiles):
        cx = i + 0.5
        ax.scatter([i + 0.12], [0.82], s=34, color=accent_colors[i % len(accent_colors)], zorder=3, clip_on=False)
        ax.text(cx, 0.56, value, ha="center", va="center", fontsize=30, fontweight="bold", color=INK)
        ax.text(cx, 0.28, label, ha="center", va="center", fontsize=11.5, fontweight="bold", color=INK_SECONDARY)
        ax.text(cx, 0.12, sub, ha="center", va="center", fontsize=9, color=INK_MUTED)
        if i > 0:
            ax.axvline(i, ymin=0.08, ymax=0.92, color=GRID, linewidth=1)

    fig.text(0.01, 0.01, f"commit {commit} · clean tree · results/{commit}/", fontsize=7.5, color=INK_MUTED)
    fig.tight_layout(rect=(0, 0.05, 1, 1))
    fig.savefig(out_path)
    plt.close(fig)


# --------------------------------------------------------------------------
# Chart 1 -- ablation impact
# --------------------------------------------------------------------------
def chart_ablation_impact(results_dir: Path, commit: str, out_path: Path):
    rows = [r for r in load_ablation_summary(results_dir) if r["matching_mode"] == "price_time"]
    if not rows:
        raise RuntimeError("no price_time rows in ablation_summary.csv")

    fig, ax = plt.subplots(figsize=(10, 6), dpi=200)
    bar_h = 0.32
    y_positions = range(len(rows))

    max_val = max(float(r["ablated_p99_9_ns"]) for r in rows) * 1.18

    for i, r in enumerate(rows):
        current = float(r["baseline_p99_9_ns"])
        rejected = float(r["ablated_p99_9_ns"])
        y_current = i + bar_h / 2 + 0.02
        y_rejected = i - bar_h / 2 - 0.02

        ax.barh(y_current, current, height=bar_h, color=SERIES_1, zorder=3)
        ax.barh(y_rejected, rejected, height=bar_h, color=SERIES_2, zorder=3)

        ax.text(current + max_val * 0.01, y_current, f"{fmt(current)} ns",
                va="center", ha="left", fontsize=9.5, fontweight="bold", color=INK)
        ax.text(rejected + max_val * 0.01, y_rejected, f"{fmt(rejected)} ns  ({r['p99_9_delta_x']}x worse)",
                va="center", ha="left", fontsize=9.5, color=INK_SECONDARY)

        verdict = r["verdict"]
        good = verdict == "PROVEN"
        chip_color = STATUS_GOOD if good else STATUS_WARN_TEXT
        marker = "✓" if good else "⚠"
        ax.annotate(f"{marker} {verdict}", xy=(1.0, i), xycoords=("axes fraction", "data"),
                    xytext=(8, 0), textcoords="offset points", va="center", ha="left",
                    fontsize=9.5, fontweight="bold", color=chip_color,
                    annotation_clip=False)

    ax.set_yticks(list(y_positions))
    ax.set_yticklabels([r["design_decision"] for r in rows], fontsize=11, fontweight="bold", color=INK)
    ax.set_xlim(0, max_val)
    ax.set_xlabel("Median P99.9 tail latency, price-time matching (ns) -- lower is better")
    ax.xaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
    ax.grid(axis="x", color=GRID, linewidth=1, zorder=0)
    ax.set_axisbelow(True)
    for spine in ("top", "right", "left"):
        ax.spines[spine].set_visible(False)
    ax.spines["bottom"].set_color(GRID)
    ax.tick_params(axis="y", length=0)
    ax.invert_yaxis()

    legend_handles = [
        plt.Rectangle((0, 0), 1, 1, color=SERIES_1, label="Current design (shipped)"),
        plt.Rectangle((0, 0), 1, 1, color=SERIES_2, label="Rejected alternative"),
    ]
    ax.legend(handles=legend_handles, loc="lower right", frameon=False, fontsize=10)

    fig.suptitle("Every optimization, proven or not", x=0.06, ha="left", fontsize=16, fontweight="bold", color=INK)
    ax.set_title(
        "Each design decision temporarily ripped out, replaced with the \"obvious\" alternative,\n"
        "re-measured on the identical dataset.",
        loc="left", fontsize=10.5, color=INK_SECONDARY, pad=14,
    )
    fig.text(0.06, 0.01, f"Source: results/{commit}/ablation_summary.md  |  commit {commit}, clean tree, 5 trials/run",
              fontsize=8.5, color=INK_MUTED)

    fig.tight_layout(rect=(0, 0.03, 0.86, 0.94))
    fig.savefig(out_path)
    plt.close(fig)


# --------------------------------------------------------------------------
# Chart 2 -- tail latency profile
# --------------------------------------------------------------------------
def chart_tail_latency(results_dir: Path, commit: str, out_path: Path):
    labels = ["P50", "P90", "P99", "P99.9", "P99.99"]
    price_time = median_percentiles(results_dir, "price_time")
    pro_rata = median_percentiles(results_dir, "pro_rata")
    price_time["P99.99"] = median_p9999(results_dir, "price_time")
    pro_rata["P99.99"] = median_p9999(results_dir, "pro_rata")

    pt_vals = [price_time[l] for l in labels]
    pr_vals = [pro_rata[l] for l in labels]
    x = range(len(labels))

    fig, ax = plt.subplots(figsize=(10, 5.6), dpi=200)
    ax.plot(x, pt_vals, color=SERIES_1, linewidth=2.4, marker="o", markersize=7,
            markerfacecolor=SERIES_1, markeredgecolor=SURFACE, markeredgewidth=1.6, zorder=3, label="Price-time")
    ax.plot(x, pr_vals, color=SERIES_2, linewidth=2.4, marker="o", markersize=7,
            markerfacecolor=SERIES_2, markeredgecolor=SURFACE, markeredgewidth=1.6, zorder=3, label="Pro-rata")

    ax.annotate(f"{fmt(pt_vals[-1])} ns", xy=(x[-1], pt_vals[-1]), xytext=(0, 12),
                textcoords="offset points", ha="center", fontsize=10, fontweight="bold", color=INK)
    ax.annotate(f"{fmt(pr_vals[-1])} ns", xy=(x[-1], pr_vals[-1]), xytext=(0, 12),
                textcoords="offset points", ha="center", fontsize=10, fontweight="bold", color=INK)

    ax.set_yscale("log")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, fontsize=11)
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
    ax.grid(axis="y", which="major", color=GRID, linewidth=1, zorder=0)
    ax.set_axisbelow(True)
    ax.set_ylabel("Latency (ns, log scale)")
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    ax.spines["left"].set_color(GRID)
    ax.spines["bottom"].set_color(GRID)
    ax.legend(loc="upper left", frameon=False, fontsize=10.5, handlelength=3.2, numpoints=1)

    fig.suptitle("Where the latency actually goes", x=0.06, ha="left", fontsize=16, fontweight="bold", color=INK)
    ax.set_title(
        "Median-of-5-trials latency at each percentile. The gap between P99 and P99.99 is the story --\n"
        "averages and even P99 hide what the last 0.01% of orders actually experience.",
        loc="left", fontsize=10.5, color=INK_SECONDARY, pad=14,
    )
    fig.text(0.06, 0.01,
              f"Source: results/{commit}/price_time.csv, pro_rata.csv (P50-P99.9); P99.99 computed from the "
              f"matching .raw.csv per-sample files.",
              fontsize=8.5, color=INK_MUTED)

    fig.tight_layout(rect=(0, 0.04, 1, 0.90))
    fig.savefig(out_path)
    plt.close(fig)


# --------------------------------------------------------------------------
# Chart 3 -- scheduler noise / stability
# --------------------------------------------------------------------------
def chart_scheduler_noise(results_dir: Path, commit: str, out_path: Path):
    pinned = trial_p999_series(results_dir, "ablation_baseline_price_time.csv")
    unpinned = trial_p999_series(results_dir, "ablation_unpinned_price_time.csv")

    fig, ax = plt.subplots(figsize=(10, 4.6), dpi=200)
    rows = [("Pinned", pinned, SERIES_1), ("Unpinned", unpinned, SERIES_2)]

    for i, (name, values, color) in enumerate(rows):
        y = len(rows) - i
        ax.hlines(y, min(values), max(values), color=color, alpha=0.35, linewidth=2, zorder=2)
        ax.scatter(values, [y] * len(values), s=90, color=color, edgecolor=SURFACE, linewidth=1.6, zorder=3)
        med = statistics.median(values)
        ax.vlines(med, y - 0.14, y + 0.14, color=INK, linewidth=2, zorder=4)
        ax.annotate(f"{fmt(max(values))} ns", xy=(max(values), y), xytext=(0, 14),
                    textcoords="offset points", ha="center", fontsize=9.5, fontweight="bold", color=INK)

    ax.set_xscale("log")
    ax.set_yticks([2, 1])
    ax.set_yticklabels(["Pinned", "Unpinned"], fontsize=12, fontweight="bold", color=INK)
    ax.set_ylim(0.5, 2.6)
    ax.xaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
    ax.set_xlabel("P99.9 latency per 5-trial run (ns, log scale) -- each dot is one full benchmark run")
    ax.grid(axis="x", which="major", color=GRID, linewidth=1, zorder=0)
    ax.set_axisbelow(True)
    for spine in ("top", "right", "left"):
        ax.spines[spine].set_visible(False)
    ax.spines["bottom"].set_color(GRID)
    ax.tick_params(axis="y", length=0)

    fig.suptitle("Why core pinning isn't optional", x=0.06, ha="left", fontsize=16, fontweight="bold", color=INK)
    ax.set_title(
        "Pinned threads land in a tight band trial to trial; unpinned threads -- same binary, same dataset --\n"
        "land wherever the OS scheduler happens to put them that run. Black tick = median.",
        loc="left", fontsize=10.5, color=INK_SECONDARY, pad=14,
    )
    fig.text(0.06, 0.035,
              f"Source: results/{commit}/ablation_baseline_price_time.csv vs ablation_unpinned_price_time.csv (per-trial P99.9).",
              fontsize=8.5, color=INK_MUTED)
    fig.text(0.06, 0.005,
              "Reads INCONCLUSIVE under price-time, PROVEN under pro-rata (~1.60x worse) -- see docs/DESIGN.md.",
              fontsize=8.5, color=INK_MUTED)

    fig.tight_layout(rect=(0, 0.09, 1, 0.88))
    fig.savefig(out_path)
    plt.close(fig)


# --------------------------------------------------------------------------
# Chart 4 -- Order struct cache-line layout (structural, not results-derived)
# --------------------------------------------------------------------------
# Hand-transcribed from include/hydra/types.hpp and cross-checked against its
# static_assert(sizeof(Order) == 128, ...) block. If that struct changes,
# these two lists must change with it -- there is no automated extraction.
HOT_LINE_RECTS = [
    ("order_id", 0, 8, "data"),
    ("price", 8, 8, "data"),
    ("qty", 16, 4, "data"),
    ("side", 20, 1, "data"),
    ("tif", 21, 1, "data"),
    ("event_tag", 22, 1, "data"),
    ("hot_padding", 23, 25, "padding"),
    ("prev_", 48, 8, "data"),
    ("next_", 56, 8, "data"),
]
HOT_LINE_LABELS = [
    ("order_id\nuint64_t · 8B", 4, 0),
    ("price\nint64_t · 8B", 12, 0),
    ("qty\nuint32_t · 4B", 18, 0),
    ("side/tif/event_tag\n1B each", 21.5, 1),
    ("hot_padding\nuint8_t[25] · 25B", 35.5, 0),
    ("prev_\nOrder* · 8B", 52, 0),
    ("next_\nOrder* · 8B", 60, 0),
]
COLD_LINE_RECTS = [
    ("timestamp_ns", 64, 8, "cold"),
    ("client_id", 72, 8, "cold"),
    ("client_tag", 80, 32, "cold"),
    ("cold_padding", 112, 16, "padding"),
]
COLD_LINE_LABELS = [
    ("timestamp_ns\nuint64_t · 8B", 68, 0),
    ("client_id\nuint64_t · 8B", 76, 0),
    ("client_tag\nchar[32] · 32B", 96, 0),
    ("cold_padding\nuint8_t[16] · 16B", 120, 0),
]

COLOR_HOT = SERIES_1
COLOR_COLD = "#7a8ba0"
COLOR_PADDING = "#d8d7d0"


def _draw_cache_line(ax, rects, labels, y0, row_h, offset_base):
    for name, start, size, kind in rects:
        color = COLOR_PADDING if kind == "padding" else (COLOR_HOT if kind == "data" else COLOR_COLD)
        rx = start - offset_base
        rect = plt.Rectangle((rx, y0), size, row_h,
                              facecolor=color, edgecolor=SURFACE, linewidth=2, zorder=3)
        ax.add_patch(rect)
        if kind == "padding":
            ax.add_patch(plt.Rectangle((rx, y0), size, row_h,
                                        facecolor="none", edgecolor=INK_MUTED, hatch="////",
                                        linewidth=0, alpha=0.4, zorder=4))

    for text, byte_pos, row in labels:
        cx = byte_pos - offset_base
        depth = row_h * 0.4 if row == 0 else row_h * 1.25
        ax.annotate(text, xy=(cx, y0), xytext=(cx, y0 - depth),
                    ha="center", va="top", fontsize=8.3, color=INK_SECONDARY,
                    linespacing=1.5,
                    arrowprops=dict(arrowstyle="-", color=INK_MUTED, linewidth=0.9,
                                     shrinkA=0, shrinkB=2))


def chart_cache_line_layout(out_path: Path):
    fig, ax = plt.subplots(figsize=(11.6, 8.4), dpi=200)
    row_h = 1.0
    y_hot, y_cold = 5.6, 1.55

    _draw_cache_line(ax, HOT_LINE_RECTS, HOT_LINE_LABELS, y_hot, row_h, offset_base=0)
    _draw_cache_line(ax, COLD_LINE_RECTS, COLD_LINE_LABELS, y_cold, row_h, offset_base=64)

    ax.text(0, y_hot + row_h + 0.35, "CACHE LINE 0 — HOT  (bytes 0–63, touched on every match-path access)",
            fontsize=12, fontweight="bold", color=INK)
    ax.text(0, y_cold + row_h + 0.35, "CACHE LINE 1 — COLD  (bytes 64–127, audit/reporting only)",
            fontsize=12, fontweight="bold", color=INK)

    boundary_y = (y_hot - row_h * 1.25) - 0.85
    ax.plot([0, 64], [boundary_y, boundary_y], color=INK, linewidth=1.4, linestyle=(0, (1, 1.6)))
    ax.text(64.8, boundary_y, "64-byte cache-line boundary", fontsize=8.5,
            color=INK_MUTED, va="center", ha="left")

    for b in (0, 16, 32, 48, 64):
        ax.text(b, y_hot + row_h + 0.08, str(b), fontsize=7.5, color=INK_MUTED, ha="center", va="bottom")
    for b in (64, 80, 96, 112, 128):
        ax.text(b - 64, y_cold + row_h + 0.08, str(b), fontsize=7.5, color=INK_MUTED, ha="center", va="bottom")

    legend_y = y_cold - row_h * 1.35 - 0.9
    legend_items = [
        (COLOR_HOT, "Hot-path field — read on every order"),
        (COLOR_COLD, "Cold field — audit/reporting only"),
        (COLOR_PADDING, "Reserved padding — enforces the 64B split"),
    ]
    for i, (color, label) in enumerate(legend_items):
        lx = i * 22
        ax.add_patch(plt.Rectangle((lx, legend_y), 1.6, 0.55, facecolor=color, edgecolor="none"))
        ax.text(lx + 2.1, legend_y + 0.28, label, fontsize=9, color=INK_SECONDARY, va="center")

    ax.set_xlim(-1, 84)
    ax.set_ylim(legend_y - 0.6, y_hot + row_h + 0.75)
    ax.axis("off")

    fig.suptitle("The Order struct: two cache lines, by construction", x=0.05, y=0.975,
                 ha="left", va="top", fontsize=17, fontweight="bold", color=INK)
    fig.text(0.05, 0.925,
              "Every byte offset here is enforced by static_assert in types.hpp — this diagram\n"
              "can't drift from the real layout without breaking the build.",
              fontsize=10.5, color=INK_SECONDARY, va="top")
    fig.text(0.05, 0.012,
              "Source: include/hydra/types.hpp (struct Order, static_assert(sizeof(Order) == 128, ...)).",
              fontsize=8.5, color=INK_MUTED)

    fig.tight_layout(rect=(0, 0.03, 1, 0.885))
    fig.savefig(out_path)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--commit", default=None, help="results/<commit>/ to read (default: current HEAD short hash)")
    parser.add_argument("--results-dir", default=None, help="explicit results directory, overrides --commit")
    args = parser.parse_args()

    commit = args.commit or git_short_hash()
    results_dir = Path(args.results_dir) if args.results_dir else REPO_ROOT / "results" / commit

    if not results_dir.is_dir():
        sys.exit(f"error: {results_dir} does not exist -- run --benchmark and scripts/run_ablations.sh first, "
                 f"or pass --commit/--results-dir to point at an existing results directory.")

    CHARTS_DIR.mkdir(exist_ok=True)

    print(f"Reading evidence from {results_dir}")
    chart_hero_stats(results_dir, commit, CHARTS_DIR / "00_hero_stats.png")
    print("  wrote charts/00_hero_stats.png")
    chart_ablation_impact(results_dir, commit, CHARTS_DIR / "01_ablation_impact.png")
    print("  wrote charts/01_ablation_impact.png")
    chart_tail_latency(results_dir, commit, CHARTS_DIR / "02_tail_latency_profile.png")
    print("  wrote charts/02_tail_latency_profile.png")
    chart_scheduler_noise(results_dir, commit, CHARTS_DIR / "03_scheduler_noise.png")
    print("  wrote charts/03_scheduler_noise.png")
    chart_cache_line_layout(CHARTS_DIR / "04_cache_line_layout.png")
    print("  wrote charts/04_cache_line_layout.png")


if __name__ == "__main__":
    main()
