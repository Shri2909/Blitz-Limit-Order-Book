#!/usr/bin/env python3
"""
Phase 6: the remaining five alpha candidates from the original list (OBI
and OFI already done in phase 5) -- generated for completeness at the
user's request, same IC/decay/quantile/split-half/orthogonality discipline
as phase 5, scored against both signed forward return (direction) and,
for spread width specifically, |forward return| (magnitude -- spread
isn't expected to be directional, so it gets the fair version of the test).

  1. Multi-level order book imbalance (MLOBI) -- same idea as L1 OBI but
     summed across all 10 displayed levels. Does looking deeper in the
     book add anything L1 doesn't already have?
  2. Micro-price deviation -- size-weighted mid vs. simple mid. A
     documented short-horizon predictor distinct from either OBI or OFI.
  3. Signed trade-flow momentum -- trailing 5s sum of signed EXECUTED
     volume (lit fills only). Distinct from OBI/OFI: this is realized
     flow, not resting-book state or state-change.
  4. Queue depletion rate -- net rate at which touch-level size is being
     consumed on one side vs. the other, trailing 5s window.
  5. Spread-width -- tested against both direction (expected null) and
     magnitude of forward return (a legitimate different question: does a
     wide spread predict upcoming volatility, independent of direction?).

Orthogonality is checked against OFI specifically (phase 5's strongest
signal) -- the real question for the bot design isn't "is this signal
correlated with the mid" in isolation, it's "does this add anything OFI
doesn't already provide."

Numpy-only, consistent with phases 1-5.
"""

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_dataset import DATASETS_DIR, SESSION_END_S, SESSION_START_S, TICKERS  # noqa: E402
from analyze_dataset_alpha import (  # noqa: E402
    HORIZONS_S,
    QUANTILE_HORIZON_S,
    compute_ofi_events,
    pearson_corr,
    spearman_ic,
)

import csv

MOMENTUM_WINDOW_S = 5
DEPLETION_WINDOW_S = 5


def stream_depth(ticker: str):
    """Superset of phase 3/5's stream_full: adds all 10 levels' sizes
    (needed for MLOBI), not just L1."""
    msg_path = DATASETS_DIR / f"{ticker}_2012-06-21_message_10.csv"
    ob_path = DATASETS_DIR / f"{ticker}_2012-06-21_orderbook_10.csv"

    times, etypes, sizes, prices, directions = [], [], [], [], []
    ask_p1, bid_p1 = [], []
    ask_sz_rows, bid_sz_rows = [], []

    with open(msg_path, newline="") as mf, open(ob_path, newline="") as of:
        for mrow, orow in zip(csv.reader(mf), csv.reader(of)):
            times.append(float(mrow[0]))
            etypes.append(int(mrow[1]))
            sizes.append(int(mrow[3]))
            prices.append(int(mrow[4]) / 1e4)
            directions.append(int(mrow[5]))
            ask_p1.append(int(orow[0]) / 1e4)
            bid_p1.append(int(orow[2]) / 1e4)
            ask_sz_rows.append([int(orow[4 * lvl + 1]) for lvl in range(10)])
            bid_sz_rows.append([int(orow[4 * lvl + 3]) for lvl in range(10)])

    return dict(
        times=np.array(times),
        etypes=np.array(etypes),
        sizes=np.array(sizes),
        prices=np.array(prices),
        directions=np.array(directions),
        ask_p1=np.array(ask_p1),
        bid_p1=np.array(bid_p1),
        ask_sz=np.array(ask_sz_rows),  # shape (n, 10)
        bid_sz=np.array(bid_sz_rows),
    )


def grid_index(times, grid):
    return np.clip(np.searchsorted(times, grid, side="right") - 1, 0, len(times) - 1)


def ic_decay_curve(signal_grid, mid_grid, label, target_fn=lambda r: r):
    row = []
    for h in HORIZONS_S:
        raw_fwd = np.full_like(mid_grid, np.nan)
        raw_fwd[:-h] = mid_grid[h:] - mid_grid[:-h]
        fwd = target_fn(raw_fwd)
        valid = ~np.isnan(fwd) & ~np.isnan(signal_grid)
        x, y = signal_grid[valid], fwd[valid]
        p = pearson_corr(x, y)
        s = spearman_ic(x, y)
        row.append((h, p, s, int(valid.sum())))
    print("    " + "  ".join(f"{h}s: P={p:+.4f}/S={s:+.4f}" for h, p, s, n in row))
    return row


def quantile_table(signal_grid, mid_grid, horizon_s, label, target_fn=lambda r: r, n_q=10):
    raw_fwd = np.full_like(mid_grid, np.nan)
    raw_fwd[:-horizon_s] = mid_grid[horizon_s:] - mid_grid[:-horizon_s]
    fwd = target_fn(raw_fwd)
    valid = ~np.isnan(fwd) & ~np.isnan(signal_grid)
    x, y = signal_grid[valid], fwd[valid]
    edges = np.percentile(x, np.linspace(0, 100, n_q + 1))
    edges[0] -= 1e-9
    edges[-1] += 1e-9
    bucket = np.digitize(x, edges[1:-1])
    means = [float(y[bucket == b].mean()) if np.any(bucket == b) else float("nan") for b in range(n_q)]
    populated = [m for m in means if not np.isnan(m)]
    monotonic = all(a <= b + 1e-12 for a, b in zip(populated, populated[1:])) or all(
        a >= b - 1e-12 for a, b in zip(populated, populated[1:])
    )
    print(f"  {label} quantile @{horizon_s}s: " + " ".join(f"{m:+.3f}" for m in means) +
          f"  -> {'monotonic' if monotonic else 'NOT monotonic'}")


def split_half_ic(signal_grid, mid_grid, grid_times, horizon_s, t_mid, label, target_fn=lambda r: r):
    def ic_for(mask):
        sig, mid = signal_grid[mask], mid_grid[mask]
        if len(mid) <= horizon_s:
            return float("nan")
        raw_fwd = np.full_like(mid, np.nan)
        raw_fwd[:-horizon_s] = mid[horizon_s:] - mid[:-horizon_s]
        fwd = target_fn(raw_fwd)
        valid = ~np.isnan(fwd) & ~np.isnan(sig)
        return pearson_corr(sig[valid], fwd[valid])

    ic_h1 = ic_for(grid_times < t_mid)
    ic_h2 = ic_for(grid_times >= t_mid)
    same_sign = (ic_h1 > 0) == (ic_h2 > 0)
    print(f"  {label} split-half @{horizon_s}s: H1={ic_h1:+.4f}  H2={ic_h2:+.4f}  "
          f"-> {'consistent' if same_sign else 'SIGN FLIPS'}")


def report_signal(signal_grid, mid_grid, grid_times, t_mid, label, ofi_grid, target_fn=lambda r: r):
    print(f"[{label}]")
    print(f"  decay curve (horizon: Pearson/Spearman):")
    ic_decay_curve(signal_grid, mid_grid, label, target_fn)
    quantile_table(signal_grid, mid_grid, QUANTILE_HORIZON_S, label, target_fn)
    split_half_ic(signal_grid, mid_grid, grid_times, QUANTILE_HORIZON_S, t_mid, label, target_fn)
    valid_ortho = ~np.isnan(signal_grid) & ~np.isnan(ofi_grid)
    corr_ofi = pearson_corr(signal_grid[valid_ortho], ofi_grid[valid_ortho])
    print(f"  corr({label}, OFI) = {corr_ofi:+.4f}  "
          f"({'redundant with OFI' if abs(corr_ofi) > 0.6 else 'distinct from OFI' if abs(corr_ofi) < 0.3 else 'partially overlapping with OFI'})")


def analyze_alpha2(ticker: str):
    print(f"\n{'=' * 70}\n{ticker} -- alpha research, phase 6 (remaining 5 signals)\n{'=' * 70}")
    d = stream_depth(ticker)
    mids_l1 = (d["ask_p1"] + d["bid_p1"]) / 2.0
    grid = np.arange(SESSION_START_S, SESSION_END_S, 1.0)
    idx_last = grid_index(d["times"], grid)
    t_mid = (SESSION_START_S + SESSION_END_S) / 2.0
    mid_grid = mids_l1[idx_last]

    # OFI grid, for orthogonality comparisons (recomputed here rather than
    # imported from phase 5's run to keep this script self-contained)
    e = compute_ofi_events({"ask_p": d["ask_p1"], "ask_sz": d["ask_sz"][:, 0],
                             "bid_p": d["bid_p1"], "bid_sz": d["bid_sz"][:, 0]})
    bucket_idx = np.clip(np.searchsorted(grid, d["times"], side="right") - 1, 0, len(grid) - 1)
    ofi_grid = np.zeros(len(grid))
    np.add.at(ofi_grid, bucket_idx, e)

    # ---- 1. multi-level OBI ----
    bid_total = d["bid_sz"].sum(axis=1)
    ask_total = d["ask_sz"].sum(axis=1)
    mlobi_raw = (bid_total - ask_total) / np.maximum(bid_total + ask_total, 1)
    mlobi_grid = mlobi_raw[idx_last]
    report_signal(mlobi_grid, mid_grid, grid, t_mid, "MLOBI (10-level)", ofi_grid)

    # ---- 2. micro-price deviation ----
    bid_sz1, ask_sz1 = d["bid_sz"][:, 0], d["ask_sz"][:, 0]
    micro = (bid_sz1 * d["ask_p1"] + ask_sz1 * d["bid_p1"]) / np.maximum(bid_sz1 + ask_sz1, 1)
    micro_dev_raw = (micro - mids_l1) * 100.0  # cents
    micro_dev_grid = micro_dev_raw[idx_last]
    report_signal(micro_dev_grid, mid_grid, grid, t_mid, "Micro-price deviation", ofi_grid)

    # ---- 3. signed trade-flow momentum (trailing 5s sum of signed lit volume) ----
    is_lit = d["etypes"] == 4
    signed_vol = np.zeros(len(d["times"]))
    signed_vol[is_lit] = -d["directions"][is_lit] * d["sizes"][is_lit]  # aggressor sign * size
    flow_1s = np.zeros(len(grid))
    np.add.at(flow_1s, bucket_idx, signed_vol)
    cum_flow = np.cumsum(flow_1s)
    momentum_grid = np.empty(len(grid))
    momentum_grid[:MOMENTUM_WINDOW_S] = cum_flow[:MOMENTUM_WINDOW_S]
    momentum_grid[MOMENTUM_WINDOW_S:] = cum_flow[MOMENTUM_WINDOW_S:] - cum_flow[:-MOMENTUM_WINDOW_S]
    report_signal(momentum_grid, mid_grid, grid, t_mid, f"Trade-flow momentum ({MOMENTUM_WINDOW_S}s trailing)", ofi_grid)

    # ---- 4. queue depletion rate (trailing 5s, net ask-minus-bid depletion) ----
    ask_sz1_grid, bid_sz1_grid = ask_sz1[idx_last], bid_sz1[idx_last]
    w = DEPLETION_WINDOW_S
    ask_depletion = np.full(len(grid), np.nan)
    bid_depletion = np.full(len(grid), np.nan)
    ask_depletion[w:] = ask_sz1_grid[:-w] - ask_sz1_grid[w:]
    bid_depletion[w:] = bid_sz1_grid[:-w] - bid_sz1_grid[w:]
    depletion_grid = ask_depletion - bid_depletion  # +ve = ask depleting faster -> bullish
    # the first w grid points have no trailing window and stay NaN -- ic_decay_curve/
    # quantile_table/split_half_ic all mask ~np.isnan(signal_grid) too, so these are
    # properly excluded rather than silently treated as "neutral" (0) observations
    report_signal(depletion_grid, mid_grid, grid, t_mid, f"Queue depletion rate ({w}s trailing)", ofi_grid)

    # ---- 5. spread width (direction AND magnitude) ----
    spread_grid = (d["ask_p1"][idx_last] - d["bid_p1"][idx_last]) * 100.0  # cents
    print("[Spread width -- direction]")
    ic_decay_curve(spread_grid, mid_grid, "Spread(dir)")
    print("[Spread width -- magnitude of forward move, |return|]")
    ic_decay_curve(spread_grid, mid_grid, "Spread(mag)", target_fn=np.abs)
    quantile_table(spread_grid, mid_grid, QUANTILE_HORIZON_S, "Spread(mag)", target_fn=np.abs)
    split_half_ic(spread_grid, mid_grid, grid, QUANTILE_HORIZON_S, t_mid, "Spread(mag)", target_fn=np.abs)


def main():
    for t in TICKERS:
        analyze_alpha2(t)


if __name__ == "__main__":
    sys.exit(main())
