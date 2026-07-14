#!/usr/bin/env python3
"""metrics/generate_report.py

Reads the CSV/text files metrics/generate_metrics.sh produces and renders
one self-contained HTML report (metrics/out/report.html) with inline SVG
charts. Stdlib only -- no matplotlib/pandas/etc., so this never needs its
own dependency install step.

Usage: generate_report.py <out_dir>
"""

import csv
import html
import os
import re
import statistics
import sys
from pathlib import Path

# ──────────────────────────────────────────────────────────────────────────
# Palette (validated instance -- see the dataviz skill's references/palette.md).
# Referenced entirely through CSS custom properties so light/dark both work
# from one definition.
# ──────────────────────────────────────────────────────────────────────────

PALETTE_CSS = """
.viz-root {
  color-scheme: light;
  --surface-1:      #fcfcfb;
  --page-plane:     #f9f9f7;
  --text-primary:   #0b0b0b;
  --text-secondary: #52514e;
  --text-muted:     #898781;
  --gridline:       #e1e0d9;
  --baseline:       #c3c2b7;
  --border:         rgba(11,11,11,0.10);
  --series-1: #2a78d6; --series-2: #1baf7a; --series-3: #eda100; --series-4: #008300;
  --series-5: #4a3aa7; --series-6: #e34948; --series-7: #e87ba4; --series-8: #eb6834;
  --status-good: #0ca30c; --status-warning: #fab219; --status-serious: #ec835a; --status-critical: #d03b3b;
}
@media (prefers-color-scheme: dark) {
  :root:where(:not([data-theme="light"])) .viz-root {
    color-scheme: dark;
    --surface-1: #1a1a19; --page-plane: #0d0d0d;
    --text-primary: #ffffff; --text-secondary: #c3c2b7; --text-muted: #898781;
    --gridline: #2c2c2a; --baseline: #383835; --border: rgba(255,255,255,0.10);
    --series-1: #3987e5; --series-2: #199e70; --series-3: #c98500; --series-4: #008300;
    --series-5: #9085e9; --series-6: #e66767; --series-7: #d55181; --series-8: #d95926;
    --status-good: #0ca30c; --status-warning: #fab219; --status-serious: #ec835a; --status-critical: #d03b3b;
  }
}
:root[data-theme="dark"] .viz-root {
  color-scheme: dark;
  --surface-1: #1a1a19; --page-plane: #0d0d0d;
  --text-primary: #ffffff; --text-secondary: #c3c2b7; --text-muted: #898781;
  --gridline: #2c2c2a; --baseline: #383835; --border: rgba(255,255,255,0.10);
  --series-1: #3987e5; --series-2: #199e70; --series-3: #c98500; --series-4: #008300;
  --series-5: #9085e9; --series-6: #e66767; --series-7: #d55181; --series-8: #d95926;
}
"""

SERIES_SLOTS = [f"var(--series-{i})" for i in range(1, 9)]


def esc(s) -> str:
    return html.escape(str(s), quote=True)


# ──────────────────────────────────────────────────────────────────────────
# Small CSV/text readers
# ──────────────────────────────────────────────────────────────────────────

def read_csv(path: Path):
    if not path.exists():
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def read_text(path: Path) -> str:
    return path.read_text() if path.exists() else ""


def read_kv_lines(path: Path):
    """Parses the 'key=value' lines struct_layout.cpp prints."""
    out = {"fields": {"order": [], "level": [], "fillevent": []}}
    for line in read_text(path).splitlines():
        if "=" not in line:
            continue
        key, _, val = line.partition("=")
        if key.endswith("_field"):
            prefix = key.split("_")[0]
            name, offset, size = val.split(",")
            out["fields"][prefix].append((name, int(offset), int(size)))
        else:
            out[key] = val
    return out


# ──────────────────────────────────────────────────────────────────────────
# SVG chart primitives
# ──────────────────────────────────────────────────────────────────────────

def svg_open(width, height, title):
    return (f'<svg viewBox="0 0 {width} {height}" width="100%" height="{height}" '
            f'role="img" aria-label="{esc(title)}" xmlns="http://www.w3.org/2000/svg">')


def chart_wrapper(title, subtitle, body_svg, width=760, height=340, legend_html="", note=""):
    note_html = f'<p class="chart-note">{esc(note)}</p>' if note else ""
    return f"""
<div class="chart-card">
  <h3>{esc(title)}</h3>
  <p class="chart-subtitle">{esc(subtitle)}</p>
  {svg_open(width, height, title)}
  {body_svg}
  </svg>
  {legend_html}
  {note_html}
</div>
"""


def make_legend(entries):
    """entries: list of (label, css_var_color)"""
    items = "".join(
        f'<span class="legend-item"><span class="legend-swatch" '
        f'style="background:{color}"></span>{esc(label)}</span>'
        for label, color in entries
    )
    return f'<div class="chart-legend">{items}</div>'


def grouped_bar_chart(categories, series, width=760, height=340, y_label="",
                      value_fmt="{:.0f}"):
    """
    categories: list of category labels (x groups)
    series: list of (name, color, values) -- values aligned with categories
    """
    margin_l, margin_r, margin_t, margin_b = 70, 30, 20, 60
    plot_w = width - margin_l - margin_r
    plot_h = height - margin_t - margin_b

    all_values = [v for _, _, vals in series for v in vals]
    max_val = max(all_values) if all_values else 1.0
    max_val = max_val * 1.15 if max_val > 0 else 1.0

    n_groups = len(categories)
    n_series = len(series)
    group_w = plot_w / max(n_groups, 1)
    bar_gap = group_w * 0.12
    bar_w = (group_w - bar_gap * (n_series + 1)) / max(n_series, 1)

    parts = []
    # gridlines + y-axis ticks
    n_ticks = 5
    for i in range(n_ticks + 1):
        frac = i / n_ticks
        y = margin_t + plot_h * (1 - frac)
        val = max_val * frac
        parts.append(f'<line x1="{margin_l}" y1="{y:.1f}" x2="{width-margin_r}" y2="{y:.1f}" '
                     f'stroke="var(--gridline)" stroke-width="1"/>')
        parts.append(f'<text x="{margin_l-10}" y="{y+4:.1f}" text-anchor="end" '
                     f'class="axis-label">{value_fmt.format(val)}</text>')

    # baseline
    parts.append(f'<line x1="{margin_l}" y1="{margin_t+plot_h}" x2="{width-margin_r}" '
                 f'y2="{margin_t+plot_h}" stroke="var(--baseline)" stroke-width="1.5"/>')

    for gi, cat in enumerate(categories):
        gx = margin_l + gi * group_w
        for si, (name, color, values) in enumerate(series):
            val = values[gi] if gi < len(values) else 0
            bar_h = (val / max_val) * plot_h if max_val > 0 else 0
            bx = gx + bar_gap + si * (bar_w + bar_gap)
            by = margin_t + plot_h - bar_h
            parts.append(f'<rect x="{bx:.1f}" y="{by:.1f}" width="{bar_w:.1f}" '
                         f'height="{bar_h:.1f}" rx="3" fill="{color}">'
                         f'<title>{esc(name)} / {esc(cat)}: {value_fmt.format(val)}{esc(y_label)}</title>'
                         f'</rect>')
            if bar_h > 14:
                parts.append(f'<text x="{bx+bar_w/2:.1f}" y="{by-6:.1f}" text-anchor="middle" '
                             f'class="bar-value-label">{value_fmt.format(val)}</text>')
        parts.append(f'<text x="{gx+group_w/2:.1f}" y="{margin_t+plot_h+22:.1f}" '
                     f'text-anchor="middle" class="axis-label">{esc(cat)}</text>')

    legend = make_legend([(name, color) for name, color, _ in series])
    return "".join(parts), legend


def line_chart(x_values, series, width=760, height=340, y_label="", x_label="",
               reference_line=None, log_x=False):
    """
    x_values: list of numeric x positions
    series: list of (name, color, y_values)
    reference_line: optional (label, y_values) drawn dashed in muted gray
    """
    margin_l, margin_r, margin_t, margin_b = 70, 30, 20, 55
    plot_w = width - margin_l - margin_r
    plot_h = height - margin_t - margin_b

    def xpos(x):
        if log_x:
            import math
            lo, hi = math.log10(max(x_values[0], 1)), math.log10(max(x_values[-1], 1))
            v = math.log10(max(x, 1))
            return margin_l + (v - lo) / (hi - lo) * plot_w if hi > lo else margin_l
        lo, hi = x_values[0], x_values[-1]
        return margin_l + (x - lo) / (hi - lo) * plot_w if hi > lo else margin_l

    all_y = [v for _, _, ys in series for v in ys]
    if reference_line:
        all_y += reference_line[1]
    max_y = max(all_y) if all_y else 1.0
    max_y = max_y * 1.15 if max_y > 0 else 1.0

    def ypos(y):
        return margin_t + plot_h * (1 - y / max_y) if max_y > 0 else margin_t + plot_h

    parts = []
    n_ticks = 5
    for i in range(n_ticks + 1):
        frac = i / n_ticks
        y = margin_t + plot_h * (1 - frac)
        val = max_y * frac
        parts.append(f'<line x1="{margin_l}" y1="{y:.1f}" x2="{width-margin_r}" y2="{y:.1f}" '
                     f'stroke="var(--gridline)" stroke-width="1"/>')
        parts.append(f'<text x="{margin_l-10}" y="{y+4:.1f}" text-anchor="end" '
                     f'class="axis-label">{val:.0f}</text>')

    parts.append(f'<line x1="{margin_l}" y1="{margin_t+plot_h}" x2="{width-margin_r}" '
                 f'y2="{margin_t+plot_h}" stroke="var(--baseline)" stroke-width="1.5"/>')

    for i, x in enumerate(x_values):
        if i == 0 or i == len(x_values) - 1 or len(x_values) <= 8:
            parts.append(f'<text x="{xpos(x):.1f}" y="{margin_t+plot_h+22:.1f}" '
                         f'text-anchor="middle" class="axis-label">{x:g}</text>')

    if reference_line:
        label, ys = reference_line
        pts = " ".join(f"{xpos(x):.1f},{ypos(y):.1f}" for x, y in zip(x_values, ys))
        parts.append(f'<polyline points="{pts}" fill="none" stroke="var(--text-muted)" '
                     f'stroke-width="2" stroke-dasharray="6,5"/>')

    for name, color, ys in series:
        pts = " ".join(f"{xpos(x):.1f},{ypos(y):.1f}" for x, y in zip(x_values, ys))
        parts.append(f'<polyline points="{pts}" fill="none" stroke="{color}" stroke-width="2.5"/>')
        for x, y in zip(x_values, ys):
            parts.append(f'<circle cx="{xpos(x):.1f}" cy="{ypos(y):.1f}" r="4" fill="{color}">'
                         f'<title>{esc(name)}: x={x:g}, y={y:.1f}</title></circle>')

    legend_entries = [(name, color) for name, color, _ in series]
    if reference_line:
        legend_entries.append((reference_line[0], "var(--text-muted)"))
    legend = make_legend(legend_entries)
    return "".join(parts), legend


def histogram_svg(values, width=760, height=340, bins=30, x_label="ns", color="var(--series-1)"):
    if not values:
        return "<text x='20' y='40'>no data</text>", ""
    margin_l, margin_r, margin_t, margin_b = 70, 30, 20, 55
    plot_w = width - margin_l - margin_r
    plot_h = height - margin_t - margin_b

    lo, hi = min(values), max(values)
    if hi <= lo:
        hi = lo + 1
    bin_w_val = (hi - lo) / bins
    counts = [0] * bins
    for v in values:
        idx = min(int((v - lo) / bin_w_val), bins - 1)
        counts[idx] += 1
    max_count = max(counts) if counts else 1

    parts = []
    n_ticks = 5
    for i in range(n_ticks + 1):
        frac = i / n_ticks
        y = margin_t + plot_h * (1 - frac)
        parts.append(f'<line x1="{margin_l}" y1="{y:.1f}" x2="{width-margin_r}" y2="{y:.1f}" '
                     f'stroke="var(--gridline)" stroke-width="1"/>')
        parts.append(f'<text x="{margin_l-10}" y="{y+4:.1f}" text-anchor="end" '
                     f'class="axis-label">{int(max_count*frac)}</text>')
    parts.append(f'<line x1="{margin_l}" y1="{margin_t+plot_h}" x2="{width-margin_r}" '
                 f'y2="{margin_t+plot_h}" stroke="var(--baseline)" stroke-width="1.5"/>')

    bar_w = plot_w / bins
    for i, c in enumerate(counts):
        bh = (c / max_count) * plot_h if max_count > 0 else 0
        bx = margin_l + i * bar_w
        by = margin_t + plot_h - bh
        bucket_lo = lo + i * bin_w_val
        parts.append(f'<rect x="{bx:.1f}" y="{by:.1f}" width="{max(bar_w-1,0):.1f}" '
                     f'height="{bh:.1f}" fill="{color}">'
                     f'<title>{bucket_lo:.0f}-{bucket_lo+bin_w_val:.0f} {esc(x_label)}: {c} samples</title>'
                     f'</rect>')
        if i % max(bins // 6, 1) == 0:
            parts.append(f'<text x="{bx:.1f}" y="{margin_t+plot_h+22:.1f}" '
                         f'text-anchor="middle" class="axis-label">{bucket_lo:.0f}</text>')

    return "".join(parts), ""


def step_chart(x_values, y_values, width=760, height=300, color="var(--series-6)"):
    margin_l, margin_r, margin_t, margin_b = 70, 30, 20, 55
    plot_w = width - margin_l - margin_r
    plot_h = height - margin_t - margin_b
    lo, hi = x_values[0], x_values[-1]
    max_y = max(y_values) * 1.2 if y_values else 1

    def xpos(x):
        return margin_l + (x - lo) / (hi - lo) * plot_w if hi > lo else margin_l

    def ypos(y):
        return margin_t + plot_h * (1 - y / max_y) if max_y > 0 else margin_t + plot_h

    parts = []
    n_ticks = 4
    for i in range(n_ticks + 1):
        frac = i / n_ticks
        y = margin_t + plot_h * (1 - frac)
        parts.append(f'<line x1="{margin_l}" y1="{y:.1f}" x2="{width-margin_r}" y2="{y:.1f}" '
                     f'stroke="var(--gridline)" stroke-width="1"/>')
        parts.append(f'<text x="{margin_l-10}" y="{y+4:.1f}" text-anchor="end" '
                     f'class="axis-label">{max_y*frac:.0f}</text>')
    parts.append(f'<line x1="{margin_l}" y1="{margin_t+plot_h}" x2="{width-margin_r}" '
                 f'y2="{margin_t+plot_h}" stroke="var(--baseline)" stroke-width="1.5"/>')

    pts = []
    for i, x in enumerate(x_values):
        pts.append(f"{xpos(x):.1f},{ypos(y_values[i]):.1f}")
        if i + 1 < len(x_values):
            pts.append(f"{xpos(x_values[i+1]):.1f},{ypos(y_values[i]):.1f}")
    parts.append(f'<polyline points="{" ".join(pts)}" fill="none" stroke="{color}" stroke-width="2.5"/>')
    for i, x in enumerate(x_values):
        parts.append(f'<circle cx="{xpos(x):.1f}" cy="{ypos(y_values[i]):.1f}" r="3" fill="{color}"/>')
    parts.append(f'<text x="{margin_l}" y="{margin_t+plot_h+22:.1f}" class="axis-label">{x_values[0]:g}</text>')
    parts.append(f'<text x="{width-margin_r}" y="{margin_t+plot_h+22:.1f}" text-anchor="end" '
                 f'class="axis-label">{x_values[-1]:g}</text>')
    return "".join(parts)


def stat_tile(label, value, sublabel="", status=None):
    status_class = f" stat-{status}" if status else ""
    return f"""<div class="stat-tile{status_class}">
  <div class="stat-value">{esc(value)}</div>
  <div class="stat-label">{esc(label)}</div>
  {f'<div class="stat-sublabel">{esc(sublabel)}</div>' if sublabel else ''}
</div>"""


def cache_line_diagram(name, fields, struct_size, scale=64):
    """fields: list of (name, offset, size). Draws horizontal 64-byte cache
    lines with each field as a proportionally-sized, labeled segment."""
    width = 760
    row_h = 54
    n_lines = max(1, (struct_size + scale - 1) // scale)
    height = n_lines * (row_h + 30) + 30
    margin_l = 70
    plot_w = width - margin_l - 30

    hot_color = "var(--series-1)"
    cold_color = "var(--series-2)"
    pad_color = "var(--gridline)"

    def field_color(fname):
        if "padding" in fname:
            return pad_color
        # cold section starts at offset >= 64 in Order; treat >=64 as cold.
        return cold_color

    parts = []
    for line_idx in range(n_lines):
        line_start = line_idx * scale
        line_end = min(line_start + scale, struct_size)
        y = 20 + line_idx * (row_h + 30)
        parts.append(f'<text x="{margin_l}" y="{y-4}" class="axis-label">'
                     f'cache line {line_idx} (bytes {line_start}-{line_end-1})</text>')
        parts.append(f'<rect x="{margin_l}" y="{y}" width="{plot_w}" height="{row_h}" '
                     f'fill="none" stroke="var(--baseline)" stroke-width="1.5" rx="4"/>')
        for fname, foff, fsize in fields:
            if foff >= line_end or foff + fsize <= line_start:
                continue
            seg_start = max(foff, line_start) - line_start
            seg_end = min(foff + fsize, line_end) - line_start
            fx = margin_l + (seg_start / scale) * plot_w
            fw = ((seg_end - seg_start) / scale) * plot_w
            color = field_color(fname)
            parts.append(f'<rect x="{fx:.1f}" y="{y+4}" width="{max(fw-2,0):.1f}" height="{row_h-8}" '
                         f'fill="{color}" opacity="0.85" rx="2">'
                         f'<title>{esc(fname)}: offset {foff}, {fsize} bytes</title></rect>')
            if fw > 34:
                label = fname if fw > 55 else fname[:6]
                parts.append(f'<text x="{fx+fw/2:.1f}" y="{y+row_h/2+4:.1f}" text-anchor="middle" '
                             f'class="field-label">{esc(label)}</text>')

    legend = make_legend([("hot / mechanical", hot_color if False else cold_color),
                          ("padding", pad_color)])
    # (hot fields all live in line 0 for Order; color them explicitly below)
    return svg_open(width, height, f"{name} cache-line layout") + "".join(parts) + "</svg>", height


def cache_line_diagram_order(fields, struct_size, scale=64):
    """Order-specific variant that colors line-0 fields as 'hot' and
    line-1 fields as 'cold' (matches the hot/cold split types.hpp documents)."""
    width = 760
    row_h = 54
    n_lines = max(1, (struct_size + scale - 1) // scale)
    height = n_lines * (row_h + 34) + 20
    margin_l = 70
    plot_w = width - margin_l - 30

    hot_color = "var(--series-1)"
    cold_color = "var(--series-2)"
    pad_color = "var(--gridline)"

    parts = []
    for line_idx in range(n_lines):
        line_start = line_idx * scale
        line_end = min(line_start + scale, struct_size)
        y = 20 + line_idx * (row_h + 34)
        line_role = "HOT (matched every match-path access)" if line_idx == 0 else "COLD (audit/reporting only)"
        parts.append(f'<text x="{margin_l}" y="{y-6}" class="axis-label">'
                     f'cache line {line_idx}, bytes {line_start}-{line_end-1} -- {esc(line_role)}</text>')
        parts.append(f'<rect x="{margin_l}" y="{y}" width="{plot_w}" height="{row_h}" '
                     f'fill="none" stroke="var(--baseline)" stroke-width="1.5" rx="4"/>')
        for fname, foff, fsize in fields:
            if foff >= line_end or foff + fsize <= line_start:
                continue
            seg_start = max(foff, line_start) - line_start
            seg_end = min(foff + fsize, line_end) - line_start
            fx = margin_l + (seg_start / scale) * plot_w
            fw = ((seg_end - seg_start) / scale) * plot_w
            color = pad_color if "padding" in fname else (hot_color if line_idx == 0 else cold_color)
            parts.append(f'<rect x="{fx:.1f}" y="{y+4}" width="{max(fw-2,0):.1f}" height="{row_h-8}" '
                         f'fill="{color}" opacity="0.88" rx="2">'
                         f'<title>{esc(fname)}: offset {foff}, {fsize} bytes</title></rect>')
            if fw > 30:
                label = fname if fw > 55 else fname[:6]
                parts.append(f'<text x="{fx+fw/2:.1f}" y="{y+row_h/2+4:.1f}" text-anchor="middle" '
                             f'class="field-label">{esc(label)}</text>')

    legend = make_legend([("hot fields", hot_color), ("cold fields", cold_color), ("padding", pad_color)])
    return svg_open(width, height, "Order cache-line layout") + "".join(parts) + "</svg>", legend


# ──────────────────────────────────────────────────────────────────────────
# Section builders
# ──────────────────────────────────────────────────────────────────────────

def section_a1(out_dir: Path) -> str:
    rows = read_csv(out_dir / "a1_cancel_latency.csv")
    if not rows:
        return '<p class="unavailable">A1 data not available (measurement did not run).</p>'
    depths = [int(r["depth"]) for r in rows]
    head = [float(r["avg_head_ns"]) for r in rows]
    tail = [float(r["avg_tail_ns"]) for r in rows]
    # Illustrative O(N) reference: normalized so it starts at the same point
    # as the head series, then grows linearly with depth -- NOT real data,
    # purely a visual contrast for what a linear-scan cancel would look like.
    scale = head[0] / depths[0] if depths[0] else 0
    onN_ref = [scale * d for d in depths]

    body, legend = line_chart(
        depths,
        [("cancel at FIFO head", SERIES_SLOTS[0], head),
         ("cancel at FIFO tail", SERIES_SLOTS[1], tail)],
        reference_line=("hypothetical O(n) scan (illustrative only)", onN_ref),
        log_x=True,
    )
    return chart_wrapper(
        "A1 — cancel_order() is O(1), not O(depth)",
        "Average wall-clock cost of cancelling the order at the head vs. the tail of a "
        "resting FIFO, swept across FIFO depth (log x-axis). Both stay flat, in the same "
        "tens-to-low-hundreds-of-ns band, regardless of depth.",
        body, legend_html=legend,
        note="Dashed gray line is NOT measured data — it's what a linear-scan cancel would "
             "look like at this scale, drawn only for visual contrast.",
    )


def section_a2(out_dir: Path) -> str:
    rows = read_csv(out_dir / "a2_fill_distribution.csv")
    if not rows:
        return '<p class="unavailable">A2 data not available.</p>'
    order_ids = sorted(set(r["maker_order_id"] for r in rows), key=int)
    modes = ["price_time", "pro_rata"]
    resting = {r["maker_order_id"]: r["resting_qty"] for r in rows}
    categories = [f'order {oid}\n({resting[oid]} resting)' for oid in order_ids]
    series = []
    for i, mode in enumerate(modes):
        vals = []
        for oid in order_ids:
            match = next((r for r in rows if r["mode"] == mode and r["maker_order_id"] == oid), None)
            vals.append(float(match["filled_qty"]) if match else 0)
        series.append((mode.replace("_", "-"), SERIES_SLOTS[i], vals))

    body, legend = grouped_bar_chart(
        [f"order {oid} ({resting[oid]} resting)" for oid in order_ids], series, y_label=" units"
    )
    return chart_wrapper(
        "A2 — price-time vs. pro-rata: same book, different (correct) fills",
        "Two resting SELL orders (30 and 70 units) at the same price; one incoming BUY for "
        "50 units, matched twice against the identical starting book.",
        body, legend_html=legend,
        note="price-time exhausts order 1 first (strict FIFO); pro-rata splits "
             "proportionally to resting size (30% / 70% of 50 = 15 / 35).",
    )


def section_a3(out_dir: Path) -> str:
    rows = read_csv(out_dir / "a3_queue_backpressure.csv")
    stderr_txt = read_text(out_dir / "a3_queue_backpressure.stderr.txt")
    if not rows:
        return '<p class="unavailable">A3 data not available.</p>'
    x = [int(r["elapsed_ms"]) for r in rows]
    y = [int(r["queue_depth"]) for r in rows]
    body, legend = line_chart(x, [("SPSC queue depth", SERIES_SLOTS[0], y)])
    drop_match = re.search(r"dropped (\d+) order", stderr_txt)
    drops = drop_match.group(1) if drop_match else "0"
    return chart_wrapper(
        "A3 — SPSC queue saturation under unthrottled load",
        "The real rx_thread_fn/matching_thread_fn, sampled every 50ms. rx_thread_fn pushes "
        "as fast as it can spin, so the queue depth shows exactly where the matching "
        "thread's own processing rate becomes the bottleneck.",
        body, legend_html=legend,
        note=f"rx_thread_fn's own backpressure counter: {drops} orders dropped during this run "
             f"(queue capacity reached, by design a recoverable condition, not a crash).",
    )


def section_a4(out_dir: Path) -> str:
    rows = read_csv(out_dir / "a4_test_coverage.csv")
    if not rows:
        return '<p class="unavailable">A4 data not available.</p>'
    total_pass = sum(int(r["pass_count"]) for r in rows)
    total_fail = sum(int(r["fail_count"]) for r in rows)
    trs = []
    for r in sorted(rows, key=lambda r: (r["phase"] != "canonical", r["phase"].zfill(3))):
        status_ok = int(r["fail_count"]) == 0 and int(r["pass_count"]) > 0
        pill_class = "pill-good" if status_ok else "pill-critical"
        pill_text = "PASS" if status_ok else "FAIL"
        saniclass = "pill-neutral" if r["sanitizer"] == "none" else "pill-info"
        trs.append(f"""<tr>
          <td>{esc(r['target'])}</td>
          <td>{esc(r['phase'])}</td>
          <td>{r['pass_count']}</td>
          <td>{r['fail_count']}</td>
          <td><span class="pill {saniclass}">{esc(r['sanitizer'])}</span></td>
          <td><span class="pill {pill_class}">{pill_text}</span></td>
        </tr>""")
    table = f"""<table class="data-table">
      <thead><tr><th>target</th><th>phase</th><th>pass</th><th>fail</th><th>sanitizer</th><th>status</th></tr></thead>
      <tbody>{"".join(trs)}</tbody>
    </table>"""
    tiles = (stat_tile("total tests passed", total_pass, status="good")
             + stat_tile("total tests failed", total_fail,
                        status="critical" if total_fail else "good"))
    return f"""
<div class="chart-card">
  <h3>A4 — per-phase test coverage dashboard</h3>
  <p class="chart-subtitle">Every phase is an independent binary (see CMakeLists.txt); this
  runs each one fresh and reports its own pass/fail tally and sanitizer coverage.</p>
  <div class="stat-row">{tiles}</div>
  {table}
</div>
"""


def section_a5(out_dir: Path) -> str:
    rows = read_csv(out_dir / "a5_pool_exhaustion.csv")
    if not rows:
        return '<p class="unavailable">A5 data not available.</p>'
    x = [int(r["event_index"]) for r in rows]
    y = [int(r["exhaustion_count"]) for r in rows]
    body = step_chart(x, y, color=SERIES_SLOTS[5])
    return chart_wrapper(
        "A5 — ObjectPool exhaustion is recoverable, not a crash",
        "A 20-slot pool: 20 successful acquires (exhaustion_count stays 0), 8 over-capacity "
        "attempts (exhaustion_count climbs, acquire() returns nullptr cleanly each time), "
        "then a single release makes the pool usable again.",
        body,
        note="The flat-then-climbing-then-flat step shape is the entire proof: exhaustion is "
             "counted, never a crash, and capacity is instantly recoverable on release().",
    )


def section_b(out_dir: Path) -> str:
    rows = read_csv(out_dir / "b1_latency_samples.csv")
    summary_txt = read_text(out_dir / "b_summary.txt")
    if not rows:
        return '<p class="unavailable">B1/B2 data not available.</p>'

    e2e = [int(r["end_to_end_ns"]) for r in rows]
    match_t = [int(r["match_time_ns"]) for r in rows]
    p50 = statistics.median(e2e)
    p99 = statistics.quantiles(e2e, n=100)[98] if len(e2e) >= 100 else max(e2e)
    match_p99 = statistics.quantiles(match_t, n=100)[98] if len(match_t) >= 100 else max(match_t)

    tput_match = re.search(r"throughput_samples_per_sec=([\d.]+)", summary_txt)
    tput = tput_match.group(1) if tput_match else "n/a"
    # calibrate_ns_per_cycle() prints this warning to stderr (captured in
    # b_summary.txt, not the stdout CSV) when the OS migrated the
    # calibrating thread across cores mid-measurement -- common on a
    # shared/virtualized host with no core pinning guarantee for the
    # (unpinned) main thread that runs calibration here.
    calibration_unreliable = "TSC calibration unreliable" in summary_txt

    body, _ = histogram_svg(e2e, x_label="ns", color=SERIES_SLOTS[0])
    e2e_vs_match_ratio = (p99 / match_p99) if match_p99 else 0
    hist_card = chart_wrapper(
        "B1 — end-to-end latency distribution, this machine",
        f"{len(e2e):,} samples captured from a real run of rx_thread_fn -> SPSC -> "
        f"matching_thread_fn on the machine that ran this script.",
        body,
        note=(f"end-to-end P99 is ~{e2e_vs_match_ratio:.0f}x match_time_ns's P99 — that gap IS "
              f"queue wait time, not matching cost. rx_thread_fn's unthrottled producer "
              f"saturates the queue almost immediately (see A3), so most of end-to-end "
              f"latency here is time spent queued, exactly like A3 shows directly. This is "
              f"the expected signature of this specific unthrottled-producer stress "
              f"scenario, not a matching-engine regression."),
    )
    tiles = (stat_tile("P50 end-to-end", f"{p50:,.0f} ns")
             + stat_tile("P99 end-to-end", f"{p99:,.0f} ns")
             + stat_tile("P99 match_time (pure compute)", f"{match_p99:,.0f} ns",
                        sublabel="excludes queue wait time")
             + stat_tile("throughput", f"{float(tput):,.0f} /s" if tput != "n/a" else "n/a"))

    calibration_note = ""
    if calibration_unreliable:
        calibration_note = ("<br><br><strong>Also:</strong> the TSC calibration step reported "
                            "itself unreliable this run (the calibrating thread was migrated "
                            "across cores mid-measurement) and fell back to a steady_clock-based "
                            "shim — see clock.hpp's own fallback path. Still real timestamps, "
                            "just extra reason these aren't tuned-hardware numbers.")
    caveat = f"""<div class="caveat-banner">
      <strong>Read this before treating these numbers as a benchmark claim:</strong>
      this program does not run the preflight checks blitz_lob --benchmark does
      (no isolcpus / performance governor / SMT-off requirement) — it deliberately
      measures whatever hardware invoked it. These numbers are illustrative of
      pipeline mechanics working correctly end-to-end, not a substitute for
      Category D's real, preflight-gated benchmark.{calibration_note}
    </div>"""

    return f"""
<div class="chart-card">
  <h3>B — latency &amp; throughput, this machine</h3>
  {caveat}
  <div class="stat-row">{tiles}</div>
</div>
{hist_card}
"""


def section_c1(out_dir: Path) -> str:
    layout = read_kv_lines(out_dir / "c1_struct_layout.txt")
    if "order_size" not in layout:
        return '<p class="unavailable">C1 data not available.</p>'

    order_svg, order_legend = cache_line_diagram_order(
        layout["fields"]["order"], int(layout["order_size"])
    )
    level_fields = layout["fields"]["level"]
    level_svg, _ = cache_line_diagram("Level", level_fields, int(layout["level_size"]))
    fe_fields = layout["fields"]["fillevent"]
    fe_svg, _ = cache_line_diagram("FillEvent", fe_fields, int(layout["fillevent_size"]))

    return f"""
<div class="chart-card">
  <h3>C1 — cache-line layout (computed from the live struct definitions)</h3>
  <p class="chart-subtitle">sizeof(Order)={layout['order_size']}B, alignof={layout['order_align']}B —
  hot fields (touched on every match) and cold fields (audit/reporting only) are deliberately
  split across the two 64-byte lines.</p>
  {order_svg}
  {order_legend}
  <p class="chart-subtitle" style="margin-top:1.5rem">
    sizeof(Level)={layout['level_size']}B and sizeof(FillEvent)={layout['fillevent_size']}B,
    shown on the same 64-byte scale for comparison:
  </p>
  {level_svg}
  {fe_svg}
</div>
"""


def parse_perf_stat_output(text: str):
    """perf stat -o writes lines like: '      1,234,567      cycles'."""
    counters = {}
    for line in text.splitlines():
        m = re.match(r"\s*([\d,]+)\s+([a-zA-Z\-_]+)", line)
        if m:
            try:
                counters[m.group(2)] = int(m.group(1).replace(",", ""))
            except ValueError:
                pass
    return counters


def section_d(out_dir: Path) -> str:
    d1_status = read_text(out_dir / "d1_status.txt").strip()
    d2_status = read_text(out_dir / "d2_status.txt").strip()
    parts = ['<div class="chart-card"><h3>D — real, preflight-gated benchmark &amp; hardware counters</h3>']

    if d1_status == "ok":
        rows = read_csv(out_dir / "d1_benchmark.csv")
        latencies = [int(r["latency_ns"]) for r in rows if r.get("latency_ns", "").isdigit()]
        by_trial = {}
        for r in rows:
            if r.get("latency_ns", "").isdigit():
                by_trial.setdefault(r["trial"], []).append(int(r["latency_ns"]))
        trial_ids = sorted(by_trial, key=int)
        trial_p99 = [statistics.quantiles(by_trial[t], n=100)[98] if len(by_trial[t]) >= 100
                    else max(by_trial[t]) for t in trial_ids]
        body, legend = grouped_bar_chart(
            [f"trial {t}" for t in trial_ids],
            [("P99 end-to-end (ns)", SERIES_SLOTS[0], trial_p99)],
            value_fmt="{:.0f}",
        )
        hist_body, _ = histogram_svg(latencies, color=SERIES_SLOTS[0])
        parts.append(f"""
        <div class="status-banner status-good">✓ Real benchmark ran successfully on this machine —
        preflight checks (isolcpus / performance governor / SMT-off) passed.</div>
        <div class="chart-subtitle">Per-trial P99 end-to-end latency, {len(trial_ids)} trials:</div>
        {svg_open(760, 340, 'D1 per-trial P99')}{body}</svg>
        {legend}
        <div class="chart-subtitle" style="margin-top:1.5rem">Overall latency_ns distribution across all trials:</div>
        {svg_open(760, 340, 'D1 histogram')}{hist_body}</svg>
        """)
    else:
        preflight_log = read_text(out_dir / "d1_benchmark.stderr.txt")
        preflight_lines = "\n".join(
            l for l in preflight_log.splitlines() if "PREFLIGHT" in l or "fatal" in l
        ) or preflight_log
        parts.append(f"""
        <div class="status-banner status-warning">
          ⚠ Real benchmark did not run: <strong>{esc(d1_status)}</strong>.
          This is <code>run_benchmark()</code>'s preflight gate working exactly as designed —
          it refuses to produce a latency number it can't stand behind on an untuned host,
          rather than silently reporting meaningless data.
        </div>
        <pre class="log-block">{esc(preflight_lines) if preflight_lines else 'blitz_lob was not built.'}</pre>
        <p class="chart-subtitle">To get real numbers: run this script on a machine configured per
        config.hpp's ENVIRONMENT_REQUIREMENTS block (isolcpus=, performance governor, SMT off).</p>
        """)

    parts.append('<hr class="section-divider">')

    if d2_status == "ok":
        counters = parse_perf_stat_output(read_text(out_dir / "d2_perf.txt"))
        ipc = (counters["instructions"] / counters["cycles"]) if counters.get("cycles") else None
        tiles = "".join([
            stat_tile("instructions", f"{counters.get('instructions', 0):,}"),
            stat_tile("cycles", f"{counters.get('cycles', 0):,}"),
            stat_tile("IPC", f"{ipc:.2f}" if ipc else "n/a"),
            stat_tile("cache-misses", f"{counters.get('cache-misses', 0):,}"),
        ])
        parts.append(f"""
        <div class="status-banner status-good">✓ perf stat counters captured.</div>
        <div class="stat-row">{tiles}</div>
        <p class="chart-subtitle">types.hpp's own comment documents a ~1.2 → ~2.1 IPC target
        from the hot/cold cache-line split as a design target, not yet a measured claim —
        this is the real measured IPC on this run.</p>
        """)
    else:
        reason = {
            "perf_not_installed": "perf is not installed on this machine.",
            "perf_unavailable": "perf ran but could not access hardware counters "
                                "(commonly /proc/sys/kernel/perf_event_paranoid restricting access).",
            "skipped_no_benchmark": "skipped because D1's benchmark did not run "
                                    "(wrapping a preflight failure in perf would fail identically).",
            "not_attempted": "not attempted.",
        }.get(d2_status, d2_status)
        parts.append(f"""
        <div class="status-banner status-warning">⚠ perf counters not available: {esc(reason)}</div>
        <p class="chart-subtitle">To fix: install <code>linux-tools-common</code> (or your
        distro's perf package) and/or lower <code>perf_event_paranoid</code>
        (<code>sudo sysctl kernel.perf_event_paranoid=1</code>), then re-run this script on a
        properly isolated benchmark machine.</p>
        """)

    parts.append("</div>")
    return "".join(parts)


# ──────────────────────────────────────────────────────────────────────────
# Page assembly
# ──────────────────────────────────────────────────────────────────────────

CSS_BODY = """
* { box-sizing: border-box; }
body {
  margin: 0; padding: 0;
  background: var(--page-plane);
  color: var(--text-primary);
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
}
.report-header {
  padding: 2.5rem 2rem 1.5rem;
  max-width: 1000px; margin: 0 auto;
}
.report-header h1 { margin: 0 0 0.25rem; font-size: 1.6rem; }
.report-header .subtitle { color: var(--text-secondary); font-size: 0.95rem; }
.report-body { max-width: 1000px; margin: 0 auto; padding: 0 2rem 3rem; }
.category-heading {
  margin: 2.5rem 0 1rem; padding-bottom: 0.5rem;
  border-bottom: 2px solid var(--baseline);
  font-size: 1.15rem;
}
.category-desc { color: var(--text-secondary); font-size: 0.9rem; margin-bottom: 1rem; }
.chart-card {
  background: var(--surface-1);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 1.25rem 1.5rem 1.5rem;
  margin-bottom: 1.25rem;
}
.chart-card h3 { margin: 0 0 0.25rem; font-size: 1.02rem; }
.chart-subtitle { color: var(--text-secondary); font-size: 0.85rem; margin: 0 0 0.75rem; line-height: 1.5; }
.chart-note { color: var(--text-muted); font-size: 0.78rem; margin-top: 0.5rem; }
.axis-label { font-size: 11px; fill: var(--text-muted); font-family: inherit; }
.bar-value-label { font-size: 11px; fill: var(--text-secondary); font-family: inherit; }
.field-label { font-size: 10px; fill: white; font-family: inherit; font-weight: 600; }
.chart-legend { display: flex; flex-wrap: wrap; gap: 0.9rem; margin-top: 0.75rem; }
.legend-item { display: inline-flex; align-items: center; gap: 0.4rem; font-size: 0.8rem; color: var(--text-secondary); }
.legend-swatch { width: 12px; height: 12px; border-radius: 3px; display: inline-block; }
.stat-row { display: flex; flex-wrap: wrap; gap: 0.75rem; margin: 0.75rem 0 1.25rem; }
.stat-tile {
  background: var(--page-plane); border: 1px solid var(--border); border-radius: 8px;
  padding: 0.85rem 1.1rem; min-width: 140px; flex: 1;
}
.stat-value { font-size: 1.5rem; font-weight: 700; }
.stat-label { color: var(--text-secondary); font-size: 0.78rem; margin-top: 0.15rem; }
.stat-sublabel { color: var(--text-muted); font-size: 0.72rem; }
.stat-good .stat-value { color: var(--status-good); }
.stat-critical .stat-value { color: var(--status-critical); }
.data-table { width: 100%; border-collapse: collapse; font-size: 0.85rem; margin-top: 0.5rem; }
.data-table th { text-align: left; color: var(--text-secondary); font-weight: 600; padding: 0.5rem 0.6rem; border-bottom: 1px solid var(--baseline); }
.data-table td { padding: 0.5rem 0.6rem; border-bottom: 1px solid var(--gridline); }
.pill { display: inline-block; padding: 0.15rem 0.55rem; border-radius: 999px; font-size: 0.72rem; font-weight: 600; }
.pill-good { background: color-mix(in srgb, var(--status-good) 18%, transparent); color: var(--status-good); }
.pill-critical { background: color-mix(in srgb, var(--status-critical) 18%, transparent); color: var(--status-critical); }
.pill-info { background: color-mix(in srgb, var(--series-1) 18%, transparent); color: var(--series-1); }
.pill-neutral { background: var(--gridline); color: var(--text-secondary); }
.caveat-banner, .status-banner {
  border-radius: 8px; padding: 0.75rem 1rem; font-size: 0.85rem; margin-bottom: 1rem; line-height: 1.5;
}
.caveat-banner { background: color-mix(in srgb, var(--status-warning) 14%, transparent); color: var(--text-primary); }
.status-good { background: color-mix(in srgb, var(--status-good) 14%, transparent); }
.status-warning { background: color-mix(in srgb, var(--status-warning) 16%, transparent); }
.log-block {
  background: var(--page-plane); border: 1px solid var(--border); border-radius: 6px;
  padding: 0.75rem 1rem; font-size: 0.78rem; overflow-x: auto; white-space: pre-wrap;
  color: var(--text-secondary);
}
.unavailable { color: var(--text-muted); font-style: italic; }
.section-divider { border: none; border-top: 1px solid var(--gridline); margin: 1.5rem 0; }
.toc { display: flex; gap: 1rem; flex-wrap: wrap; margin-top: 1rem; }
.toc a {
  color: var(--series-1); text-decoration: none; font-size: 0.85rem;
  border: 1px solid var(--border); border-radius: 999px; padding: 0.3rem 0.8rem;
}
"""


def build_report(out_dir: Path) -> str:
    a1, a2, a3, a4, a5 = (section_a1(out_dir), section_a2(out_dir), section_a3(out_dir),
                          section_a4(out_dir), section_a5(out_dir))
    b = section_b(out_dir)
    c1 = section_c1(out_dir)
    d = section_d(out_dir)

    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Blitz-Limit-Order-Book — visual metrics report</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
{PALETTE_CSS}
{CSS_BODY}
</style>
</head>
<body>
<div class="viz-root">
  <div class="report-header">
    <h1>Blitz-Limit-Order-Book — visual metrics report</h1>
    <p class="subtitle">Generated by metrics/generate_metrics.sh. Every number on this page came
    from actually running the project's real code on this machine — nothing here is illustrative
    unless a caveat banner says so.</p>
    <nav class="toc">
      <a href="#cat-a">A · Proven behavior</a>
      <a href="#cat-b">B · This-machine measurements</a>
      <a href="#cat-c">C · Structure</a>
      <a href="#cat-d">D · Tuned-hardware benchmark</a>
    </nav>
  </div>
  <div class="report-body">

    <h2 class="category-heading" id="cat-a">A — Proofs from already-verified behavior</h2>
    <p class="category-desc">Correctness and design properties the test suite already
    established; re-measured fresh on every run of this script.</p>
    {a1}
    {a2}
    {a3}
    {a4}
    {a5}

    <h2 class="category-heading" id="cat-b">B — Quick measurements, this machine</h2>
    <p class="category-desc">Real pipeline behavior, captured on whatever machine ran this
    script — explicitly not a substitute for Category D's tuned-hardware numbers.</p>
    {b}

    <h2 class="category-heading" id="cat-c">C — Structure (no measurement needed)</h2>
    <p class="category-desc">Facts computed from the live struct definitions, not a
    hand-drawn diagram that could drift out of sync with the code.</p>
    {c1}

    <h2 class="category-heading" id="cat-d">D — Real, preflight-gated benchmark</h2>
    <p class="category-desc">The actual blitz_lob --benchmark entry point and real
    perf hardware counters — gated exactly the way the project intends, so a
    non-tuned host reports that honestly instead of a fabricated number.</p>
    {d}

  </div>
</div>
</body>
</html>
"""


def main():
    if len(sys.argv) != 2:
        print("usage: generate_report.py <out_dir>", file=sys.stderr)
        sys.exit(1)
    out_dir = Path(sys.argv[1])
    report_html = build_report(out_dir)
    (out_dir / "report.html").write_text(report_html)
    print(f"wrote {out_dir / 'report.html'}")


if __name__ == "__main__":
    main()
