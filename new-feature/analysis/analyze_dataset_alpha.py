#!/usr/bin/env python3
"""
Phase 5: alpha research, restricted to the two signals worth the effort
(see conversation) -- OBI, fully characterized this time (phase 3 only
checked two correlation points), and OFI (Cont/Kukanov/Stoikov 2014's
order flow imbalance) as its most likely refinement.

Per signal, per ticker:
  - IC decay curve: Pearson AND Spearman-rank IC across horizons
    1,2,5,10,20,30,60 seconds, on a 1s-resampled grid (same resampling
    convention as every prior phase -- avoids overweighting message
    bursts, and 1s already matches the median order lifetime found in
    phase 3, so finer resolution wouldn't be actionable anyway).
  - Quantile monotonicity: mean forward return by signal decile at a
    representative horizon, to catch nonlinear or spurious-looking
    correlations a single IC number would hide.
  - Split-half IC consistency: IC computed independently on each session
    half -- the walk-forward-style check, adapted for a raw signal with
    no fitted parameters (there's nothing to "overfit" the way k's slope
    could be, so this checks sign/magnitude stability instead).

Then:
  - Orthogonality: correlation between OBI and OFI themselves -- if
    they're highly correlated, OFI is relabeling OBI, not adding
    information.

Numpy-only, consistent with phases 1-4.
"""

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_dataset import SESSION_END_S, SESSION_START_S, TICKERS  # noqa: E402
from analyze_dataset_signals import stream_full  # noqa: E402

HORIZONS_S = [1, 2, 5, 10, 20, 30, 60]
QUANTILE_HORIZON_S = 5


def pearson_corr(x, y):
    x = x - x.mean()
    y = y - y.mean()
    denom = np.sqrt((x @ x) * (y @ y))
    return float((x @ y) / denom) if denom > 0 else float("nan")


def rankdata(a):
    """Average-rank transform (matches scipy.stats.rankdata(method='average')),
    reimplemented to avoid a scipy dependency this project doesn't otherwise have."""
    sorter = np.argsort(a, kind="mergesort")
    inv = np.empty(len(a), dtype=np.intp)
    inv[sorter] = np.arange(len(a), dtype=np.intp)
    a_sorted = a[sorter]
    obs = np.r_[True, a_sorted[1:] != a_sorted[:-1]]
    dense = obs.cumsum()[inv]  # 1-indexed dense rank per element; dense.min()==1
    count = np.bincount(dense)  # count[0]==0 since dense never equals 0
    cum = np.cumsum(count)  # cum[i] = how many elements have dense-rank <= i
    return 0.5 * (cum[dense - 1] + cum[dense] + 1)


def spearman_ic(x, y):
    return pearson_corr(rankdata(x), rankdata(y))


def compute_ofi_events(d):
    """Cont/Kukanov/Stoikov (2014) order flow imbalance, per message event.
    e_n^bid = q_n*1{P_n>=P_{n-1}} - q_{n-1}*1{P_n<=P_{n-1}}  (net bid pressure)
    e_n^ask = q_n*1{P_n<=P_{n-1}} - q_{n-1}*1{P_n>=P_{n-1}}  (net ask pressure)
    OFI_n = e_n^bid - e_n^ask."""
    ask_p, ask_sz, bid_p, bid_sz = d["ask_p"], d["ask_sz"], d["bid_p"], d["bid_sz"]
    prev_bid_p = np.empty_like(bid_p); prev_bid_p[0] = bid_p[0]; prev_bid_p[1:] = bid_p[:-1]
    prev_bid_sz = np.empty_like(bid_sz); prev_bid_sz[0] = bid_sz[0]; prev_bid_sz[1:] = bid_sz[:-1]
    prev_ask_p = np.empty_like(ask_p); prev_ask_p[0] = ask_p[0]; prev_ask_p[1:] = ask_p[:-1]
    prev_ask_sz = np.empty_like(ask_sz); prev_ask_sz[0] = ask_sz[0]; prev_ask_sz[1:] = ask_sz[:-1]

    e_bid = np.where(bid_p > prev_bid_p, bid_sz,
             np.where(bid_p < prev_bid_p, -prev_bid_sz, bid_sz - prev_bid_sz))
    e_ask = np.where(ask_p < prev_ask_p, ask_sz,
             np.where(ask_p > prev_ask_p, -prev_ask_sz, ask_sz - prev_ask_sz))
    e = (e_bid - e_ask).astype(float)
    e[0] = 0.0  # row 0 has no prior state to compare against
    return e


def build_grids(d, mids):
    grid = np.arange(SESSION_START_S, SESSION_END_S, 1.0)
    idx_last = np.clip(np.searchsorted(d["times"], grid, side="right") - 1, 0, len(mids) - 1)

    obi_grid = (d["bid_sz"][idx_last] - d["ask_sz"][idx_last]) / np.maximum(
        d["bid_sz"][idx_last] + d["ask_sz"][idx_last], 1
    )
    mid_grid = mids[idx_last]

    e = compute_ofi_events(d)
    bucket_idx = np.clip(np.searchsorted(grid, d["times"], side="right") - 1, 0, len(grid) - 1)
    ofi_grid = np.zeros(len(grid))
    np.add.at(ofi_grid, bucket_idx, e)

    return grid, obi_grid, ofi_grid, mid_grid


def ic_decay_curve(signal_grid, mid_grid, label):
    print(f"  {label} decay curve (horizon: Pearson IC / Spearman IC / n):")
    row = []
    for h in HORIZONS_S:
        fwd = np.full_like(mid_grid, np.nan)
        fwd[:-h] = mid_grid[h:] - mid_grid[:-h]
        valid = ~np.isnan(fwd)
        x, y = signal_grid[valid], fwd[valid]
        p = pearson_corr(x, y)
        s = spearman_ic(x, y)
        row.append((h, p, s, int(valid.sum())))
    print("    " + "  ".join(f"{h}s: P={p:+.4f}/S={s:+.4f}" for h, p, s, n in row))
    return row


def quantile_table(signal_grid, mid_grid, horizon_s, label, n_q=10):
    fwd = np.full_like(mid_grid, np.nan)
    fwd[:-horizon_s] = mid_grid[horizon_s:] - mid_grid[:-horizon_s]
    valid = ~np.isnan(fwd)
    x, y = signal_grid[valid], fwd[valid]
    edges = np.percentile(x, np.linspace(0, 100, n_q + 1))
    edges[0] -= 1e-9
    edges[-1] += 1e-9
    bucket = np.digitize(x, edges[1:-1])
    means = [float(y[bucket == b].mean()) if np.any(bucket == b) else float("nan") for b in range(n_q)]
    populated = [m for m in means if not np.isnan(m)]  # empty deciles (tied percentile
    # boundaries, e.g. many exactly-zero OFI seconds) shouldn't break the monotonicity
    # verdict for the deciles that DO have data
    monotonic = all(a <= b + 1e-12 for a, b in zip(populated, populated[1:])) or all(
        a >= b - 1e-12 for a, b in zip(populated, populated[1:])
    )
    print(f"  {label} quantile table @{horizon_s}s (decile 1=lowest signal .. {n_q}=highest): " +
          " ".join(f"{m:+.2f}" for m in means) +
          f"   -> {'monotonic' if monotonic else 'NOT monotonic'}")


def split_half_ic(signal_grid, mid_grid, grid_times, horizon_s, t_mid, label):
    def ic_for(mask):
        sig, mid = signal_grid[mask], mid_grid[mask]
        if len(mid) <= horizon_s:
            return float("nan")
        fwd = np.full_like(mid, np.nan)
        fwd[:-horizon_s] = mid[horizon_s:] - mid[:-horizon_s]
        valid = ~np.isnan(fwd)
        return pearson_corr(sig[valid], fwd[valid])

    ic_h1 = ic_for(grid_times < t_mid)
    ic_h2 = ic_for(grid_times >= t_mid)
    same_sign = (ic_h1 > 0) == (ic_h2 > 0)
    print(f"  {label} split-half IC @{horizon_s}s: H1={ic_h1:+.4f}  H2={ic_h2:+.4f}  "
          f"-> {'consistent sign' if same_sign else 'SIGN FLIPS across halves'}")


def analyze_alpha(ticker: str):
    print(f"\n{'=' * 70}\n{ticker} -- alpha research (OBI + OFI)\n{'=' * 70}")
    d = stream_full(ticker)
    mids = (d["ask_p"] + d["bid_p"]) / 2.0
    grid, obi_grid, ofi_grid, mid_grid = build_grids(d, mids)
    t_mid = (SESSION_START_S + SESSION_END_S) / 2.0

    print(f"OFI summary: mean={ofi_grid.mean():+.1f} shares/s  std={ofi_grid.std():.1f}  "
          f"(net order-flow pressure per second; +ve = net buy-side pressure)")

    print("[OBI]")
    ic_decay_curve(obi_grid, mid_grid, "OBI")
    quantile_table(obi_grid, mid_grid, QUANTILE_HORIZON_S, "OBI")
    split_half_ic(obi_grid, mid_grid, grid, QUANTILE_HORIZON_S, t_mid, "OBI")

    print("[OFI]")
    ic_decay_curve(ofi_grid, mid_grid, "OFI")
    quantile_table(ofi_grid, mid_grid, QUANTILE_HORIZON_S, "OFI")
    split_half_ic(ofi_grid, mid_grid, grid, QUANTILE_HORIZON_S, t_mid, "OFI")

    print("[orthogonality]")
    corr = pearson_corr(obi_grid, ofi_grid)
    print(f"  corr(OBI, OFI) = {corr:+.4f}  "
          f"({'largely redundant with OBI' if abs(corr) > 0.6 else 'meaningfully distinct from OBI' if abs(corr) < 0.3 else 'partially overlapping with OBI'})")


def main():
    for t in TICKERS:
        analyze_alpha(t)


if __name__ == "__main__":
    sys.exit(main())
