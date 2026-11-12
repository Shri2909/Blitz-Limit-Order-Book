#!/usr/bin/env python3
"""Generate the five README charts from benchmark evidence in results/<commit>/.

Usage:
    python3 charts/generate_charts.py --commit 98a597b
    python3 charts/generate_charts.py --results-dir results/<commit>

Benchmark values come from the result CSVs. Test status comes from the latest local
CTest log and shows as unavailable when no usable log exists. The layout chart is
transcribed from types.hpp and must be updated when that layout changes.
"""

import argparse
import csv
import re
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

SERIES_1 = "#2a78d6"  # baseline / price-time / pinned
SERIES_2 = "#eb6834"  # rejected alternative / pro-rata / unpinned
STATUS_GOOD = "#0ca30c"
STATUS_WARN = "#8a6200"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#898781"
GRID = "#e1e0d9"
SURFACE = "#fcfcfb"

plt.rcParams.update(
    {
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
    }
)


# -- Evidence loading ---------------------------------------------------------


def git_short_hash() -> str:
    return subprocess.run(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()


def read_csv_rows(path: Path) -> list:
    with path.open() as f:
        return list(csv.DictReader(f))


def median_column(results_dir: Path, filename: str, column: str) -> float:
    return statistics.median(float(r[column]) for r in read_csv_rows(results_dir / filename))


def median_percentiles(results_dir: Path, mode: str) -> dict:
    """Median across trials of P50/P90/P99/P99.9 from <mode>.csv."""
    columns = {
        "P50": "p50_ns_per_order",
        "P90": "p90_ns_per_order",
        "P99": "p99_ns_per_order",
        "P99.9": "p999_ns_per_order",
    }
    return {
        label: median_column(results_dir, f"{mode}.csv", column)
        for label, column in columns.items()
    }


def median_p9999(results_dir: Path, mode: str) -> float:
    """Median across trials of P99.99; the summary CSV stops at P99.9, so use the raw samples."""
    trials: dict[int, list[int]] = {}
    with (results_dir / f"{mode}.raw.csv").open() as f:
        for row in csv.DictReader(f):
            if row.get("is_cancel") == "1" or row.get("is_replace") == "1":
                continue
            trials.setdefault(int(row["trial"]), []).append(int(row["end_to_end_ns"]))

    def p9999(values):
        ordered = sorted(values)
        return ordered[min(int(99.99 / 100.0 * len(ordered)), len(ordered) - 1)]

    return statistics.median(p9999(values) for values in trials.values())


def trial_p999_series(results_dir: Path, filename: str) -> list:
    return sorted(float(r["p999_ns_per_order"]) for r in read_csv_rows(results_dir / filename))


def aggregate_dropped_events(results_dir: Path) -> tuple:
    """Return (trials, dropped events) summed over every per-trial CSV in results_dir."""
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


def parse_ctest_results(log_path: Path | None = None):
    """Return (passed, total) from a usable local CTest log, or None."""
    if log_path is None:
        log_path = REPO_ROOT / "build" / "Testing" / "Temporary" / "LastTest.log"
    if not log_path.is_file():
        return None
    text = log_path.read_text(errors="ignore")
    if "Unable to find executable:" in text or re.search(r'^Command: ""$', text, re.M):
        return None
    results = re.findall(r"^Test (Passed|Failed)(?:\.|$)", text, re.M)
    if not results:
        return None
    return results.count("Passed"), len(results)


def test_status_tile(test_result):
    """Describe recorded test results without inferring sanitizer coverage."""
    if test_result is None:
        return "N/A", "test status unavailable", "run ctest to record local results"
    passed, total = test_result
    return f"{passed}/{total}", "test suites passing", "latest local CTest run"


# -- Shared plotting helpers --------------------------------------------------


def fmt(n) -> str:
    return f"{n:,.0f}"


def new_figure(width, height):
    return plt.subplots(figsize=(width, height), dpi=200)


def finish(fig, out_path: Path, rect):
    fig.tight_layout(rect=rect)
    fig.savefig(out_path)
    plt.close(fig)


def add_headline(fig, ax, title, subtitle):
    fig.suptitle(title, x=0.06, ha="left", fontsize=16, fontweight="bold", color=INK)
    ax.set_title(subtitle, loc="left", fontsize=10.5, color=INK_SECONDARY, pad=14)


def add_footnote(fig, text, y=0.01):
    fig.text(0.06, y, text, fontsize=8.5, color=INK_MUTED)


def style_axes(ax, grid_axis, hidden_spines):
    ax.grid(axis=grid_axis, which="major", color=GRID, linewidth=1, zorder=0)
    ax.set_axisbelow(True)
    for name, spine in ax.spines.items():
        if name in hidden_spines:
            spine.set_visible(False)
        else:
            spine.set_color(GRID)


def comma_format(axis):
    axis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))


# -- Charts -------------------------------------------------------------------


def chart_hero_stats(results_dir: Path, commit: str, out_path: Path):
    p999 = median_percentiles(results_dir, "price_time")["P99.9"]
    throughput = median_column(results_dir, "price_time.csv", "throughput_orders_per_second")
    total_trials, total_dropped = aggregate_dropped_events(results_dir)
    test_result = parse_ctest_results()
    tests_passed = test_result is not None and test_result[0] == test_result[1]

    tiles = [
        (f"{fmt(p999)} ns", "P99.9 tail latency", "price-time, median of 5 trials"),
        (
            f"{throughput / 1000:.0f}K/s",
            "sustained throughput",
            f"{fmt(throughput)} orders/sec, single core",
        ),
        test_status_tile(test_result),
        (f"{total_dropped}", "dropped events", f"across all {total_trials} measured trials"),
    ]
    accent_colors = [
        SERIES_1,
        SERIES_1,
        STATUS_GOOD if tests_passed else STATUS_WARN,
        STATUS_GOOD if total_dropped == 0 else STATUS_WARN,
    ]

    fig, ax = new_figure(11.4, 2.5)
    ax.set_xlim(0, len(tiles))
    ax.set_ylim(0, 1)
    ax.axis("off")

    for i, (value, label, sub) in enumerate(tiles):
        cx = i + 0.5
        ax.scatter([i + 0.12], [0.82], s=34, color=accent_colors[i], zorder=3, clip_on=False)
        ax.text(
            cx, 0.56, value, ha="center", va="center", fontsize=30, fontweight="bold", color=INK
        )
        ax.text(
            cx,
            0.28,
            label,
            ha="center",
            va="center",
            fontsize=11.5,
            fontweight="bold",
            color=INK_SECONDARY,
        )
        ax.text(cx, 0.12, sub, ha="center", va="center", fontsize=9, color=INK_MUTED)
        if i > 0:
            ax.axvline(i, ymin=0.08, ymax=0.92, color=GRID, linewidth=1)

    fig.text(
        0.01, 0.01, f"Benchmark commit {commit} · results/{commit}/", fontsize=7.5, color=INK_MUTED
    )
    finish(fig, out_path, rect=(0, 0.05, 1, 1))


def chart_ablation_impact(results_dir: Path, commit: str, out_path: Path):
    rows = [
        r
        for r in read_csv_rows(results_dir / "ablation_summary.csv")
        if r["matching_mode"] == "price_time"
    ]
    if not rows:
        raise RuntimeError("no price_time rows in ablation_summary.csv")

    fig, ax = new_figure(10, 6)
    bar_h = 0.32
    max_val = max(float(r["ablated_p99_9_ns"]) for r in rows) * 1.18

    for i, r in enumerate(rows):
        current = float(r["baseline_p99_9_ns"])
        rejected = float(r["ablated_p99_9_ns"])
        y_current = i + bar_h / 2 + 0.02
        y_rejected = i - bar_h / 2 - 0.02

        ax.barh(y_current, current, height=bar_h, color=SERIES_1, zorder=3)
        ax.barh(y_rejected, rejected, height=bar_h, color=SERIES_2, zorder=3)
        ax.text(
            current + max_val * 0.01,
            y_current,
            f"{fmt(current)} ns",
            va="center",
            ha="left",
            fontsize=9.5,
            fontweight="bold",
            color=INK,
        )
        ax.text(
            rejected + max_val * 0.01,
            y_rejected,
            f"{fmt(rejected)} ns  ({r['p99_9_delta_x']}x worse)",
            va="center",
            ha="left",
            fontsize=9.5,
            color=INK_SECONDARY,
        )

        verdict = r["verdict"]
        proven = verdict == "PROVEN"
        ax.annotate(
            f"{'✓' if proven else '⚠'} {verdict}",
            xy=(1.0, i),
            xycoords=("axes fraction", "data"),
            xytext=(8, 0),
            textcoords="offset points",
            va="center",
            ha="left",
            fontsize=9.5,
            fontweight="bold",
            color=STATUS_GOOD if proven else STATUS_WARN,
            annotation_clip=False,
        )

    ax.set_yticks(range(len(rows)))
    ax.set_yticklabels(
        [r["design_decision"] for r in rows], fontsize=11, fontweight="bold", color=INK
    )
    ax.set_xlim(0, max_val)
    ax.set_xlabel("Median P99.9 tail latency, price-time matching (ns) -- lower is better")
    comma_format(ax.xaxis)
    style_axes(ax, "x", hidden_spines=("top", "right", "left"))
    ax.tick_params(axis="y", length=0)
    ax.invert_yaxis()

    legend_handles = [
        plt.Rectangle((0, 0), 1, 1, color=SERIES_1, label="Measured baseline"),
        plt.Rectangle((0, 0), 1, 1, color=SERIES_2, label="Rejected alternative"),
    ]
    ax.legend(handles=legend_handles, loc="lower right", frameon=False, fontsize=10)

    add_headline(
        fig,
        ax,
        "Measured impact of each optimization",
        "Baseline and ablated variants, measured on the same dataset.",
    )
    add_footnote(fig, f"Source: results/{commit}/ablation_summary.md  |  benchmark commit {commit}")
    finish(fig, out_path, rect=(0, 0.03, 0.86, 0.94))


def chart_tail_latency(results_dir: Path, commit: str, out_path: Path):
    labels = ["P50", "P90", "P99", "P99.9", "P99.99"]
    series = []
    for name, mode, color in (
        ("Price-time", "price_time", SERIES_1),
        ("Pro-rata", "pro_rata", SERIES_2),
    ):
        percentiles = median_percentiles(results_dir, mode)
        percentiles["P99.99"] = median_p9999(results_dir, mode)
        series.append((name, [percentiles[label] for label in labels], color))

    x = range(len(labels))
    fig, ax = new_figure(10, 5.6)
    for name, values, color in series:
        ax.plot(
            x,
            values,
            color=color,
            linewidth=2.4,
            marker="o",
            markersize=7,
            markerfacecolor=color,
            markeredgecolor=SURFACE,
            markeredgewidth=1.6,
            zorder=3,
            label=name,
        )
    for _, values, _ in series:
        ax.annotate(
            f"{fmt(values[-1])} ns",
            xy=(x[-1], values[-1]),
            xytext=(0, 12),
            textcoords="offset points",
            ha="center",
            fontsize=10,
            fontweight="bold",
            color=INK,
        )

    ax.set_yscale("log")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, fontsize=11)
    comma_format(ax.yaxis)
    style_axes(ax, "y", hidden_spines=("top", "right"))
    ax.set_ylabel("Latency (ns, log scale)")
    ax.legend(loc="upper left", frameon=False, fontsize=10.5, handlelength=3.2, numpoints=1)

    add_headline(
        fig,
        ax,
        "Where the latency actually goes",
        "Median-of-5-trials latency at each percentile. The gap between P99 and P99.99 is the story --\n"
        "averages and even P99 hide what the last 0.01% of orders actually experience.",
    )
    add_footnote(
        fig,
        f"Source: results/{commit}/price_time.csv, pro_rata.csv (P50-P99.9); P99.99 computed from the "
        "matching .raw.csv per-sample files.",
    )
    finish(fig, out_path, rect=(0, 0.04, 1, 0.90))


def chart_scheduler_noise(results_dir: Path, commit: str, out_path: Path):
    rows = [
        ("Pinned", trial_p999_series(results_dir, "ablation_baseline_price_time.csv"), SERIES_1),
        ("Unpinned", trial_p999_series(results_dir, "ablation_unpinned_price_time.csv"), SERIES_2),
    ]

    fig, ax = new_figure(10, 4.6)
    for i, (_, values, color) in enumerate(rows):
        y = len(rows) - i
        ax.hlines(y, min(values), max(values), color=color, alpha=0.35, linewidth=2, zorder=2)
        ax.scatter(
            values, [y] * len(values), s=90, color=color, edgecolor=SURFACE, linewidth=1.6, zorder=3
        )
        median = statistics.median(values)
        ax.vlines(median, y - 0.14, y + 0.14, color=INK, linewidth=2, zorder=4)
        ax.annotate(
            f"{fmt(max(values))} ns",
            xy=(max(values), y),
            xytext=(0, 14),
            textcoords="offset points",
            ha="center",
            fontsize=9.5,
            fontweight="bold",
            color=INK,
        )

    ax.set_xscale("log")
    ax.set_yticks([2, 1])
    ax.set_yticklabels(["Pinned", "Unpinned"], fontsize=12, fontweight="bold", color=INK)
    ax.set_ylim(0.5, 2.6)
    comma_format(ax.xaxis)
    ax.set_xlabel(
        "P99.9 latency per 5-trial run (ns, log scale) -- each dot is one full benchmark run"
    )
    style_axes(ax, "x", hidden_spines=("top", "right", "left"))
    ax.tick_params(axis="y", length=0)

    add_headline(
        fig,
        ax,
        "Core pinning and trial-to-trial variation",
        "Pinned threads land in a tight band trial to trial; unpinned threads -- same binary, same dataset --\n"
        "land wherever the OS scheduler happens to put them that run. Black tick = median.",
    )
    add_footnote(
        fig,
        f"Source: results/{commit}/ablation_baseline_price_time.csv vs ablation_unpinned_price_time.csv (per-trial P99.9).",
        y=0.035,
    )
    add_footnote(fig, "Classification details are in the benchmark ablation summary.", y=0.005)
    finish(fig, out_path, rect=(0, 0.09, 1, 0.88))


# Transcribed from include/hydra/types.hpp (static_assert(sizeof(Order) == 128));
# update these lists together with that struct.
# Rect tuples are (field, byte offset, size, kind); label tuples are (text, byte, row).
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
KIND_COLORS = {"data": COLOR_HOT, "cold": COLOR_COLD, "padding": COLOR_PADDING}


def draw_cache_line(ax, rects, labels, title, y0, row_h, offset_base):
    for _, start, size, kind in rects:
        rx = start - offset_base
        ax.add_patch(
            plt.Rectangle(
                (rx, y0),
                size,
                row_h,
                facecolor=KIND_COLORS[kind],
                edgecolor=SURFACE,
                linewidth=2,
                zorder=3,
            )
        )
        if kind == "padding":
            ax.add_patch(
                plt.Rectangle(
                    (rx, y0),
                    size,
                    row_h,
                    facecolor="none",
                    edgecolor=INK_MUTED,
                    hatch="////",
                    linewidth=0,
                    alpha=0.4,
                    zorder=4,
                )
            )

    for text, byte_pos, row in labels:
        cx = byte_pos - offset_base
        depth = row_h * 0.4 if row == 0 else row_h * 1.25
        ax.annotate(
            text,
            xy=(cx, y0),
            xytext=(cx, y0 - depth),
            ha="center",
            va="top",
            fontsize=8.3,
            color=INK_SECONDARY,
            linespacing=1.5,
            arrowprops=dict(arrowstyle="-", color=INK_MUTED, linewidth=0.9, shrinkA=0, shrinkB=2),
        )

    ax.text(0, y0 + row_h + 0.35, title, fontsize=12, fontweight="bold", color=INK)
    for byte in range(offset_base, offset_base + 65, 16):
        ax.text(
            byte - offset_base,
            y0 + row_h + 0.08,
            str(byte),
            fontsize=7.5,
            color=INK_MUTED,
            ha="center",
            va="bottom",
        )


def chart_cache_line_layout(out_path: Path):
    fig, ax = new_figure(11.6, 8.4)
    row_h = 1.0
    y_hot, y_cold = 5.6, 1.55

    draw_cache_line(
        ax,
        HOT_LINE_RECTS,
        HOT_LINE_LABELS,
        "CACHE LINE 0 — HOT  (bytes 0–63, touched on every match-path access)",
        y_hot,
        row_h,
        offset_base=0,
    )
    draw_cache_line(
        ax,
        COLD_LINE_RECTS,
        COLD_LINE_LABELS,
        "CACHE LINE 1 — METADATA  (bytes 64–127, identity and timing)",
        y_cold,
        row_h,
        offset_base=64,
    )

    boundary_y = (y_hot - row_h * 1.25) - 0.85
    ax.plot([0, 64], [boundary_y, boundary_y], color=INK, linewidth=1.4, linestyle=(0, (1, 1.6)))
    ax.text(
        64.8,
        boundary_y,
        "64-byte cache-line boundary",
        fontsize=8.5,
        color=INK_MUTED,
        va="center",
        ha="left",
    )

    legend_y = y_cold - row_h * 1.35 - 0.9
    legend_items = [
        (COLOR_HOT, "Order and FIFO fields"),
        (COLOR_COLD, "Identity, timing, and client tag"),
        (COLOR_PADDING, "Reserved padding — enforces the 64B split"),
    ]
    for i, (color, label) in enumerate(legend_items):
        lx = i * 22
        ax.add_patch(plt.Rectangle((lx, legend_y), 1.6, 0.55, facecolor=color, edgecolor="none"))
        ax.text(lx + 2.1, legend_y + 0.28, label, fontsize=9, color=INK_SECONDARY, va="center")

    ax.set_xlim(-1, 84)
    ax.set_ylim(legend_y - 0.6, y_hot + row_h + 0.75)
    ax.axis("off")

    fig.suptitle(
        "The Order struct: two cache lines, by construction",
        x=0.05,
        y=0.975,
        ha="left",
        va="top",
        fontsize=17,
        fontweight="bold",
        color=INK,
    )
    fig.text(
        0.05,
        0.925,
        "Layout transcribed from types.hpp; update this diagram when the struct changes.\n"
        "Matching reads client_id on the metadata line for self-trade prevention.",
        fontsize=10.5,
        color=INK_SECONDARY,
        va="top",
    )
    fig.text(
        0.05,
        0.012,
        "Source: include/hydra/types.hpp (struct Order, static_assert(sizeof(Order) == 128, ...)).",
        fontsize=8.5,
        color=INK_MUTED,
    )
    finish(fig, out_path, rect=(0, 0.03, 1, 0.885))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--commit",
        default=None,
        help="results/<commit>/ to read (default: current HEAD short hash)",
    )
    parser.add_argument(
        "--results-dir", default=None, help="explicit results directory, overrides --commit"
    )
    args = parser.parse_args()

    commit = args.commit or git_short_hash()
    results_dir = Path(args.results_dir) if args.results_dir else REPO_ROOT / "results" / commit

    if not results_dir.is_dir():
        sys.exit(
            f"error: {results_dir} does not exist -- run --benchmark and scripts/run_ablations.sh first, "
            "or pass --commit/--results-dir to point at an existing results directory."
        )

    CHARTS_DIR.mkdir(exist_ok=True)

    print(f"Reading evidence from {results_dir}")
    charts = [
        ("00_hero_stats.png", lambda path: chart_hero_stats(results_dir, commit, path)),
        ("01_ablation_impact.png", lambda path: chart_ablation_impact(results_dir, commit, path)),
        ("02_tail_latency_profile.png", lambda path: chart_tail_latency(results_dir, commit, path)),
        ("03_scheduler_noise.png", lambda path: chart_scheduler_noise(results_dir, commit, path)),
        ("04_cache_line_layout.png", chart_cache_line_layout),
    ]
    for name, render in charts:
        render(CHARTS_DIR / name)
        print(f"  wrote charts/{name}")


if __name__ == "__main__":
    main()
