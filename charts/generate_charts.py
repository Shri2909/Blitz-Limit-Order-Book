#!/usr/bin/env python3
"""Generate the four README charts from benchmark evidence on disk.

Usage:
    python3 charts/generate_charts.py \
        --results-dir results_new/77eb286 \
        --ctest-log build/Testing/Temporary/LastTest.log

Every benchmark number comes from the result CSVs in --results-dir (the run of record).
Test status comes from the CTest log given with --ctest-log and shows N/A without one.
The layout chart is transcribed from include/hydra/types.hpp and must be updated when
that struct changes (tests/test_chart_reporting.py checks the transcription is gap-free).
"""

import argparse
import csv
import re
import statistics
import sys
import textwrap
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
from matplotlib.patches import Polygon

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_RESULTS_DIR = REPO_ROOT / "results_new" / "77eb286"
DEFAULT_OUT_DIR = Path(__file__).resolve().parent

SERIES_1 = "#2a78d6"  # price-time / baseline
SERIES_2 = "#eb6834"  # pro-rata / ablated variant
STATUS_GOOD = "#0a7d0a"
STATUS_WARN = "#8a6200"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#6e6c66"
GRID = "#e1e0d9"
SURFACE = "#fcfcfb"

PERCENTILE_COLUMNS = {
    "P50": "p50_ns_per_order",
    "P90": "p90_ns_per_order",
    "P99": "p99_ns_per_order",
    "P99.9": "p999_ns_per_order",
}
MODES = (("Price-time", "price_time", SERIES_1), ("Pro-rata", "pro_rata", SERIES_2))

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


def display_path(path: Path) -> str:
    """Repo-relative path with a trailing slash for directories, for footers."""
    try:
        text = str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        text = str(path)
    return text + "/" if path.is_dir() else text


def read_csv_rows(path: Path) -> list:
    with path.open() as f:
        return list(csv.DictReader(f))


def run_commit(results_dir: Path) -> str:
    """Commit hash recorded in the run's own canonical CSV (not the current git HEAD)."""
    return read_csv_rows(results_dir / "price_time.csv")[0]["commit_hash"]


def median_column(results_dir: Path, filename: str, column: str) -> float:
    return statistics.median(float(r[column]) for r in read_csv_rows(results_dir / filename))


def median_percentiles(results_dir: Path, mode: str) -> dict:
    """Median across trials of the per-trial P50/P90/P99/P99.9 (new orders only)."""
    return {
        label: median_column(results_dir, f"{mode}.csv", column)
        for label, column in PERCENTILE_COLUMNS.items()
    }


def median_p9999(results_dir: Path, mode: str) -> tuple:
    """(median across trials of P99.99, samples per trial); the summary CSV stops at P99.9."""
    trials: dict[int, list[int]] = {}
    with (results_dir / f"{mode}.raw.csv").open() as f:
        for row in csv.DictReader(f):
            if row.get("is_cancel") == "1" or row.get("is_replace") == "1":
                continue
            trials.setdefault(int(row["trial"]), []).append(int(row["end_to_end_ns"]))

    def p9999(values):
        ordered = sorted(values)
        return ordered[min(int(99.99 / 100.0 * len(ordered)), len(ordered) - 1)]

    value = statistics.median(p9999(values) for values in trials.values())
    return value, min(len(values) for values in trials.values())


def trial_p999_series(results_dir: Path, variant: str, mode: str = "price_time") -> list:
    """Per-trial P99.9 of one ablation arm, in trial order."""
    rows = read_csv_rows(results_dir / f"ablation_{variant}_{mode}.csv")
    return [float(r["p999_ns_per_order"]) for r in rows]


def aggregate_dropped_events(results_dir: Path) -> tuple:
    """Return (trials, dropped events) summed over every per-trial CSV in results_dir."""
    total_trials = 0
    total_dropped = 0
    for csv_path in sorted(results_dir.glob("*.csv")):
        if csv_path.name.endswith(".raw.csv") or csv_path.name == "ablation_summary.csv":
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
    """Return (passed, total) from a usable CTest LastTest.log, or None."""
    if log_path is None or not log_path.is_file():
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
        return "N/A", "test status unavailable", "no CTest log given (--ctest-log)"
    passed, total = test_result
    return f"{passed}/{total}", "ctest targets passing", "local CTest run, Release build"


# -- Ablation verdict vocabulary ----------------------------------------------

# icon, colour, short human description of what the verdict means
VERDICT_STYLE = {
    "PROVEN": ("✓", STATUS_GOOD),
    "NO_MEASURABLE_EFFECT": ("≈", INK_SECONDARY),
    "INCONCLUSIVE": ("?", STATUS_WARN),
    "ABLATED_FASTER": ("↓", STATUS_WARN),
    "INVALID": ("✗", STATUS_WARN),
}
# Names used by earlier result sets (the earlier rule called the mirror verdict REGRESSION).
LEGACY_VERDICTS = {"REGRESSION": "ABLATED_FASTER"}

# CSV design_decision -> (ablation variant file stem, chart row title)
DECISIONS = {
    "Lock-free SPSC queue": ("mutex_queue", "Lock-free SPSC queue"),
    "Core pinning + isolation": (
        "unpinned",
        "Core isolation + pinning vs unpinned (shared desktop)",
    ),
    "Fixed-slab object pooling": ("malloc_pool", "Fixed-slab object pooling"),
    "Cache-line hot/cold split": ("flat_layout", "Cache-line hot/cold split"),
}


def normalize_verdict(verdict: str) -> str:
    verdict = LEGACY_VERDICTS.get(verdict, verdict)
    if verdict not in VERDICT_STYLE:
        raise ValueError(f"unknown ablation verdict {verdict!r}")
    return verdict


def trial_range_text(trials: list) -> str:
    return f"per-trial range {fmt(min(trials))}\u2013{fmt(max(trials))} ns"


def ablated_label(
    verdict: str, ablated: float, delta_x: float, trials: list, baseline_trials: list | None = None
) -> tuple:
    """Bar-label lines for the ablated arm; 'x worse' is only claimed for PROVEN.

    A PROVEN arm whose per-trial values span more than 10x (a load-dependent tail) gets a
    third line stating the separation margin (fastest ablated trial minus slowest baseline
    trial) and that such a row can flip between runs.
    """
    verdict = normalize_verdict(verdict)
    value = f"{fmt(ablated)} ns"
    spread = trial_range_text(trials)
    if verdict == "PROVEN":
        lines = (f"{value}  ({delta_x:.2f}x worse)", spread)
        if baseline_trials and max(trials) > 10 * min(trials):
            margin = min(trials) - max(baseline_trials)
            note = (
                f"tail highly load dependent; separation margin only {fmt(margin)} ns; "
                "this row flips between runs"
            )
            return lines + tuple(textwrap.wrap(note, 45))
        return lines
    if verdict == "NO_MEASURABLE_EFFECT":
        return f"{value}  (no measurable effect)", spread
    if verdict == "ABLATED_FASTER":
        return f"{value}  (ablated variant was faster)", spread
    if verdict == "INCONCLUSIVE":
        return f"{value}  (median; inconclusive)", f"tail highly load dependent: {spread}"
    return f"{value}  (invalid run)", spread


# -- Shared plotting helpers --------------------------------------------------


def fmt(n) -> str:
    return f"{n:,.0f}"


def new_figure(width, height):
    return plt.subplots(figsize=(width, height), dpi=200)


def finish(fig, out_path: Path, rect=None):
    if rect is not None:
        fig.tight_layout(rect=rect)
    fig.savefig(out_path, metadata={"Software": None})
    plt.close(fig)


def add_headline(fig, title, subtitle, x=0.06, y=0.975, sub_y=None):
    """Left-aligned title plus a one-line (or two-line) subtitle under it, in figure space."""
    height = fig.get_figheight()
    fig.text(x, y, title, ha="left", va="top", fontsize=16, fontweight="bold", color=INK)
    sub_y = sub_y if sub_y is not None else y - 0.45 / height
    fig.text(x, sub_y, subtitle, ha="left", va="top", fontsize=10.5, color=INK_SECONDARY)


def add_footnotes(fig, lines, x=0.06, size=8.5):
    """Stack footnote lines from the bottom edge up; return the top of the stack (fig coords)."""
    height = fig.get_figheight()
    step = size * 1.55 / 72 / height
    for k, line in enumerate(reversed(lines)):
        fig.text(x, 0.012 + k * step, line, fontsize=size, color=INK_MUTED, va="bottom")
    return 0.012 + len(lines) * step + 0.008


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


def chart_hero_stats(results_dir: Path, commit: str, out_path: Path, ctest_log: Path | None = None):
    p999 = median_percentiles(results_dir, "price_time")["P99.9"]
    replay_rate = median_column(results_dir, "price_time.csv", "throughput_orders_per_second")
    total_trials, total_dropped = aggregate_dropped_events(results_dir)
    test_result = parse_ctest_results(ctest_log)
    tests_passed = test_result is not None and test_result[0] == test_result[1]

    tiles = [
        (f"{fmt(p999)} ns", "P99.9 tail latency", "price-time, median of 5 trials"),
        (
            f"{fmt(round(replay_rate, -3))}/s",
            "offered load (paced replay)",
            "orders per second; capacity not measured",
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
        0.01,
        0.01,
        f"commit {commit}  ·  {display_path(results_dir)}  ·  "
        "one host, synthetic dataset, in-process latency",
        fontsize=7.5,
        color=INK_MUTED,
    )
    finish(fig, out_path, rect=(0, 0.05, 1, 1))


def clip_cap(values: list, factor: float = 4.0) -> float:
    """Longest bar length to draw: the maximum, unless it dwarfs the runner-up by `factor`."""
    ordered = sorted(values)
    if len(ordered) > 1 and ordered[-1] > factor * ordered[-2]:
        return ordered[-2] * 1.25
    return ordered[-1]


def draw_break(ax, x, y, height):
    """Slanted surface-coloured stripe across a bar marking that the bar is cut short."""
    half = height / 2
    width = ax.get_xlim()[1] * 0.006
    slant = width * 2
    ax.add_patch(
        Polygon(
            [
                (x - width + slant, y + half),
                (x + width + slant, y + half),
                (x + width - slant, y - half),
                (x - width - slant, y - half),
            ],
            closed=True,
            facecolor=SURFACE,
            edgecolor="none",
            zorder=6,
        )
    )


def chart_ablation_impact(results_dir: Path, commit: str, out_path: Path):
    rows_by_decision = {
        r["design_decision"]: r
        for r in read_csv_rows(results_dir / "ablation_summary.csv")
        if r["matching_mode"] == "price_time"
    }
    missing = [name for name in DECISIONS if name not in rows_by_decision]
    if missing:
        raise RuntimeError(f"ablation_summary.csv has no price_time row for {missing}")
    rows = [rows_by_decision[name] for name in DECISIONS]

    baseline_trials = trial_p999_series(results_dir, "baseline")
    bar_values = [float(r["baseline_p99_9_ns"]) for r in rows] + [
        float(r["ablated_p99_9_ns"]) for r in rows
    ]
    cap = clip_cap(bar_values)
    xmax = cap * 1.5

    fig = plt.figure(figsize=(12.4, 8.4), dpi=200)
    footnotes = [
        f"Baseline here is the ablation suite's own run (median P99.9 "
        f"{fmt(float(rows[0]['baseline_p99_9_ns']))} ns); the run of record in chart 00 and 02 "
        f"is a separate run of the same binary ({fmt(median_percentiles(results_dir, 'price_time')['P99.9'])} ns).",
        "Verdicts use per-trial P99.9, 5 trials per arm. PROVEN: median >= 110% of baseline and every ablated trial "
        "above every baseline trial.",
        "NO_MEASURABLE_EFFECT: median within 10% of baseline (not detected, not proven equal). ABLATED_FASTER: the mirror of PROVEN.",
        "Pinning row: stock unpinned threads on a busy desktop vs isolated pinned cores. It measures core isolation plus "
        "placement luck, not pinning alone.",
        "Pro-rata, same suite: " + pro_rata_summary(results_dir),
        f"commit {commit}  ·  {display_path(results_dir / 'ablation_summary.csv')}  ·  "
        "one host, one session",
    ]
    bottom = add_footnotes(fig, footnotes, x=0.03)
    ax = fig.add_axes((0.29, bottom + 0.075, 0.45, 0.75 - bottom))

    bar_h = 0.26
    ax.set_xlim(0, xmax)
    for i, r in enumerate(rows):
        _, title = DECISIONS[r["design_decision"]]
        variant = DECISIONS[r["design_decision"]][0]
        verdict = normalize_verdict(r["verdict"])
        baseline = float(r["baseline_p99_9_ns"])
        ablated = float(r["ablated_p99_9_ns"])
        trials = trial_p999_series(results_dir, variant)
        y_base = i - bar_h / 2 - 0.02
        y_abl = i + bar_h / 2 + 0.02

        for value, y, color in ((baseline, y_base, SERIES_1), (ablated, y_abl, SERIES_2)):
            ax.barh(y, min(value, cap), height=bar_h, color=color, zorder=3)
            if value > cap:
                draw_break(ax, cap * 0.95, y, bar_h)
        # direct labels right of each bar: value, then the per-trial spread
        gap = xmax * 0.012
        ax.text(
            min(baseline, cap) + gap,
            y_base,
            f"{fmt(baseline)} ns\n{trial_range_text(baseline_trials)}",
            va="center",
            ha="left",
            fontsize=9.5,
            color=INK_SECONDARY,
            linespacing=1.5,
        )
        label_lines = list(
            ablated_label(verdict, ablated, float(r["p99_9_delta_x"]), trials, baseline_trials)
        )
        if ablated > cap:
            label_lines[0] = f"{label_lines[0]}  [bar cut, axis break]"
        ax.text(
            min(ablated, cap) + gap,
            y_abl if len(label_lines) <= 2 else y_abl - bar_h / 2,
            "\n".join(label_lines),
            # a long label hangs from the top of its bar so it cannot reach the baseline label
            va="center" if len(label_lines) <= 2 else "top",
            ha="left",
            fontsize=9.5,
            color=INK_SECONDARY,
            linespacing=1.5,
        )

        icon, color = VERDICT_STYLE[verdict]
        ax.annotate(
            f"{icon} {verdict}",
            xy=(1.0, i),
            xycoords=("axes fraction", "data"),
            xytext=(30, 0),
            textcoords="offset points",
            va="center",
            ha="left",
            fontsize=10,
            fontweight="bold",
            color=color,
            annotation_clip=False,
        )
        ax.annotate(
            textwrap.fill(title, 27),
            xy=(0, i),
            xycoords=("axes fraction", "data"),
            xytext=(-12, 3),
            textcoords="offset points",
            va="bottom",
            ha="right",
            fontsize=10.5,
            fontweight="bold",
            color=INK,
            annotation_clip=False,
        )
        ax.annotate(
            "vs " + textwrap.fill(r["ablated_label"], 30),
            xy=(0, i),
            xycoords=("axes fraction", "data"),
            xytext=(-12, -3),
            textcoords="offset points",
            va="top",
            ha="right",
            fontsize=9,
            color=INK_SECONDARY,
            annotation_clip=False,
        )

    ax.set_yticks([])
    ax.set_xlim(0, xmax)
    ax.set_ylim(len(rows) - 0.5, -0.5)
    ax.set_xlabel("Median P99.9 latency, price-time matching (ns). Lower is better.")
    comma_format(ax.xaxis)
    style_axes(ax, "x", hidden_spines=("top", "right", "left"))
    ax.annotate(
        "Verdict",
        xy=(1.0, 1.0),
        xycoords=("axes fraction", "axes fraction"),
        xytext=(30, 4),
        textcoords="offset points",
        ha="left",
        va="bottom",
        fontsize=9,
        color=INK_MUTED,
        annotation_clip=False,
    )

    legend_handles = [
        plt.Rectangle((0, 0), 1, 1, color=SERIES_1, label="Baseline (all optimizations on)"),
        plt.Rectangle((0, 0), 1, 1, color=SERIES_2, label="Ablated variant"),
    ]
    ax.legend(
        handles=legend_handles,
        loc="lower left",
        bbox_to_anchor=(0, 1.0),
        ncol=2,
        frameon=False,
        fontsize=9.5,
    )

    add_headline(
        fig,
        "Measured impact of each design decision",
        "Median P99.9 of 5 trials per arm, same dataset, price-time matching. A verdict says what was measured, not why.",
        x=0.03,
    )
    finish(fig, out_path)


def pro_rata_summary(results_dir: Path) -> str:
    short = {
        "Lock-free SPSC queue": "queue",
        "Core pinning + isolation": "isolation",
        "Fixed-slab object pooling": "pooling",
        "Cache-line hot/cold split": "layout",
    }
    parts = []
    for r in read_csv_rows(results_dir / "ablation_summary.csv"):
        if r["matching_mode"] == "pro_rata":
            parts.append(
                f"{short[r['design_decision']]} {float(r['p99_9_delta_x']):.2f}x "
                f"{normalize_verdict(r['verdict'])}"
            )
    return "; ".join(parts)


def chart_tail_latency(results_dir: Path, commit: str, out_path: Path):
    labels = ["P50", "P90", "P99", "P99.9", "P99.99"]
    series = []
    samples = 0
    for name, mode, color in MODES:
        percentiles = median_percentiles(results_dir, mode)
        percentiles["P99.99"], samples = median_p9999(results_dir, mode)
        series.append((name, [percentiles[label] for label in labels], color))
    per_trial_tail = samples // 10_000 + (samples % 10_000 > 0)

    x = list(range(len(labels)))
    fig, ax = new_figure(10.4, 6.2)
    for k, (name, values, color) in enumerate(series):
        above = k == 1  # pro-rata labels above its line, price-time below
        ax.plot(
            x[:-1],
            values[:-1],
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
        ax.plot(x[-2:], values[-2:], color=color, linewidth=1.8, linestyle=(0, (3, 2)), zorder=2)
        ax.plot(
            x[-1],
            values[-1],
            marker="o",
            markersize=7,
            markerfacecolor=SURFACE,
            markeredgecolor=color,
            markeredgewidth=1.8,
            zorder=3,
        )
        for j, value in enumerate(values):
            text = f"{fmt(value)} ns" if j < len(values) - 1 else f"{fmt(value)} ns (indicative)"
            ax.annotate(
                text,
                xy=(x[j], value),
                xytext=(0, 11 if above else -17),
                textcoords="offset points",
                ha="center",
                fontsize=9,
                fontweight="bold" if j == 3 else "normal",
                color=INK,
            )

    ax.set_yscale("log")
    ax.set_xlim(-0.4, len(labels) - 0.4)
    ax.set_ylim(200, 8000)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=11)
    ax.set_yticks([200, 500, 1000, 2000, 5000])
    ax.yaxis.set_minor_locator(mticker.NullLocator())
    comma_format(ax.yaxis)
    style_axes(ax, "y", hidden_spines=("top", "right"))
    ax.set_ylabel("Latency (ns, log scale)")
    ax.legend(loc="upper left", frameon=False, fontsize=10.5, handlelength=3.2, numpoints=1)

    add_headline(
        fig,
        "Where the latency goes",
        "Median of 5 trials of each per-trial percentile, new orders only. The tail, not the median, is the story.\n"
        f"P99.99 is indicative only: about {per_trial_tail} samples per trial (hollow marker, dashed).",
        y=0.975,
    )
    bottom = add_footnotes(
        fig,
        [
            f"commit {commit}  ·  {display_path(results_dir)}price_time.csv, pro_rata.csv "
            "(P50-P99.9); P99.99 from the matching .raw.csv",
            "End-to-end latency, producer timestamp to end of matching, in-process (no network or market-data path).",
        ],
    )
    fig.tight_layout(rect=(0, bottom, 1, 0.85))
    finish(fig, out_path)


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
    ("order_id\nuint64_t · 8B\n@0", 4, 0),
    ("price\nint64_t · 8B\n@8", 12, 0),
    ("qty\nuint32_t · 4B\n@16", 18, 0),
    ("side@20, tif@21, event_tag@22\n1B each", 21.5, 1),
    ("hot_padding\nuint8_t[25] · 25B", 35.5, 0),
    ("prev_\nOrder* · 8B\n@48", 52, 0),
    ("next_\nOrder* · 8B\n@56", 60, 0),
]
COLD_LINE_RECTS = [
    ("timestamp_ns", 64, 8, "cold"),
    ("client_id", 72, 8, "cold"),
    ("client_tag", 80, 32, "cold"),
    ("cold_padding", 112, 16, "padding"),
]
COLD_LINE_LABELS = [
    ("timestamp_ns\nuint64_t · 8B\n@64", 68, 0),
    ("client_id\nuint64_t · 8B\n@72", 76, 0),
    ("client_tag\nchar[32] · 32B\n@80", 96, 0),
    ("cold_padding\nuint8_t[16] · 16B\n@112", 120, 0),
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
        depth = row_h * 0.4 if row == 0 else row_h * 1.55
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


def chart_cache_line_layout(commit: str, out_path: Path):
    fig, ax = new_figure(11.6, 8.4)
    row_h = 1.0
    y_hot, y_cold = 6.1, 1.65

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

    boundary_y = (y_hot - row_h * 1.55) - 0.95
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

    legend_y = y_cold - row_h * 1.45 - 0.9
    legend_items = [
        (COLOR_HOT, "Order and FIFO fields"),
        (COLOR_COLD, "Identity, timing, and client tag"),
        (COLOR_PADDING, "Reserved padding — enforces the 64B split"),
    ]
    for i, (color, label) in enumerate(legend_items):
        lx = i * 22
        ax.add_patch(plt.Rectangle((lx, legend_y), 1.6, 0.55, facecolor=color, edgecolor="none"))
        ax.text(lx + 2.1, legend_y + 0.28, label, fontsize=9, color=INK_SECONDARY, va="center")

    caption_y = legend_y - 1.0
    ax.text(
        0,
        caption_y,
        "measured effect of the split: none on the canonical workload (see DESIGN.md)",
        fontsize=11,
        fontweight="bold",
        color=INK,
        va="center",
    )

    ax.set_xlim(-1, 84)
    ax.set_ylim(caption_y - 0.7, y_hot + row_h + 0.75)
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
        0.93,
        "Offsets in bytes, transcribed from types.hpp. Sweeps read client_id on the metadata line for\n"
        "self-trade prevention, so a sweep touches both lines of every resting order it examines.",
        fontsize=10.5,
        color=INK_SECONDARY,
        va="top",
    )
    fig.text(
        0.05,
        0.012,
        "Source: include/hydra/types.hpp (struct Order, static_assert(sizeof(Order) == 128, ...))  ·  "
        f"commit {commit}",
        fontsize=8.5,
        color=INK_MUTED,
    )
    finish(fig, out_path, rect=(0, 0.03, 1, 0.89))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=DEFAULT_RESULTS_DIR,
        help="results directory of the run of record (default: results_new/77eb286)",
    )
    parser.add_argument(
        "--out-dir", type=Path, default=DEFAULT_OUT_DIR, help="where the PNGs go (default: charts/)"
    )
    parser.add_argument(
        "--ctest-log",
        type=Path,
        default=None,
        help="CTest LastTest.log for the tests tile (without one the tile shows N/A)",
    )
    args = parser.parse_args()

    if not args.results_dir.is_dir():
        sys.exit(f"error: results directory {args.results_dir} does not exist (see --help)")

    commit = run_commit(args.results_dir)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    print(f"Reading evidence from {args.results_dir}")
    charts = [
        (
            "00_headline_stats.png",
            lambda path: chart_hero_stats(args.results_dir, commit, path, args.ctest_log),
        ),
        (
            "01_ablation_verdicts.png",
            lambda path: chart_ablation_impact(args.results_dir, commit, path),
        ),
        (
            "02_latency_percentiles.png",
            lambda path: chart_tail_latency(args.results_dir, commit, path),
        ),
        ("03_order_layout.png", lambda path: chart_cache_line_layout(commit, path)),
    ]
    for name, render in charts:
        render(args.out_dir / name)
        print(f"  wrote {args.out_dir / name}")


if __name__ == "__main__":
    main()
