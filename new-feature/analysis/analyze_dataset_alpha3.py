#!/usr/bin/env python3
"""
Phase 7: the last alpha-research step (see conversation) -- not new raw
signal-hunting, but (1) combining the signals already validated in phases
5-6, and (2) VPIN, a different KIND of signal (flow toxicity / risk, not
direction) that phase 4's simpler size-vs-markout test didn't fully cover.

Part A -- composite signal:
  OFI and trade-flow momentum are real, only partially correlated
  (phase 6: 0.17-0.36) -- a combination might beat either alone. Two
  combination methods, compared:
    (a) equal-weighted z-score sum -- the standard, no-fitting baseline.
    (b) OLS-weighted combination, weights fit on H1 ONLY, then scored on
        H2's IC -- the actual walk-forward test of whether fitting
        weights helps beyond just averaging, or overfits the combination
        itself. For INTC/MSFT, micro-price deviation joins as a third
        component (phase 6 found it comparably strong there).

Part B -- VPIN (Easley, Lopez de Prado & O'Hara):
  Buckets LIT trades by volume (not time), tracks buy/sell imbalance per
  bucket, rolling-averages it. Designed to answer "how toxic is the flow
  right now" -- i.e. when to widen or pull quotes, not which way to skew.
  Checked two ways: (i) does elevated VPIN predict higher subsequent
  volatility, and (ii) does elevated VPIN AT THE MOMENT OF A FILL predict
  worse markout on that specific fill -- the second one connects directly
  back to phase 3's markout decomposition, which is the more actionable
  question for the strategy.

Numpy-only, consistent with phases 1-6.
"""

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_dataset import SESSION_END_S, SESSION_START_S, TICKERS  # noqa: E402
from analyze_dataset_alpha import compute_ofi_events, pearson_corr, spearman_ic  # noqa: E402
from analyze_dataset_alpha2 import MOMENTUM_WINDOW_S, grid_index, stream_depth  # noqa: E402

TICKERS_WITH_MICRO = {"INTC", "MSFT"}
VPIN_N_BUCKETS = 200  # deliberately finer than Easley et al.'s 50-per-day
VPIN_ROLLING = 20     # convention -- a single session needs more resolution
                        # to get a usable sample size; noted explicitly below.


def zscore(x):
    s = x.std()
    return (x - x.mean()) / s if s > 0 else np.zeros_like(x)


def build_component_grids(d, grid, idx_last, bucket_idx):
    e = compute_ofi_events({"ask_p": d["ask_p1"], "ask_sz": d["ask_sz"][:, 0],
                             "bid_p": d["bid_p1"], "bid_sz": d["bid_sz"][:, 0]})
    ofi_grid = np.zeros(len(grid))
    np.add.at(ofi_grid, bucket_idx, e)

    is_lit = d["etypes"] == 4
    signed_vol = np.zeros(len(d["times"]))
    signed_vol[is_lit] = -d["directions"][is_lit] * d["sizes"][is_lit]
    flow_1s = np.zeros(len(grid))
    np.add.at(flow_1s, bucket_idx, signed_vol)
    cum = np.cumsum(flow_1s)
    mom_grid = np.empty(len(grid))
    mom_grid[:MOMENTUM_WINDOW_S] = cum[:MOMENTUM_WINDOW_S]
    mom_grid[MOMENTUM_WINDOW_S:] = cum[MOMENTUM_WINDOW_S:] - cum[:-MOMENTUM_WINDOW_S]

    bid_sz1, ask_sz1 = d["bid_sz"][:, 0], d["ask_sz"][:, 0]
    micro = (bid_sz1 * d["ask_p1"] + ask_sz1 * d["bid_p1"]) / np.maximum(bid_sz1 + ask_sz1, 1)
    mids_l1 = (d["ask_p1"] + d["bid_p1"]) / 2.0
    micro_dev_grid = ((micro - mids_l1) * 100.0)[idx_last]

    return ofi_grid, mom_grid, micro_dev_grid


def ic_at_horizon(signal, mid_grid, h):
    fwd = np.full_like(mid_grid, np.nan)
    fwd[:-h] = mid_grid[h:] - mid_grid[:-h]
    valid = ~np.isnan(fwd)
    return pearson_corr(signal[valid], fwd[valid]), spearman_ic(signal[valid], fwd[valid]), valid


def composite_analysis(ticker, d, grid, idx_last, bucket_idx, mid_grid, t_mid):
    print("[Composite signal]")
    ofi_grid, mom_grid, micro_grid = build_component_grids(d, grid, idx_last, bucket_idx)
    use_micro = ticker in TICKERS_WITH_MICRO
    components = {"OFI": ofi_grid, "TradeFlowMom": mom_grid}
    if use_micro:
        components["MicroDev"] = micro_grid

    component_spearman = {}
    for name, sig in components.items():
        p, s, _ = ic_at_horizon(sig, mid_grid, 5)
        component_spearman[name] = s
        print(f"  component {name:14s} IC@5s: Pearson={p:+.4f} Spearman={s:+.4f}")

    z_components = [zscore(sig) for sig in components.values()]
    equal_weight = np.sum(z_components, axis=0)
    p_eq, s_eq, _ = ic_at_horizon(equal_weight, mid_grid, 5)
    best_single = max(component_spearman.values())
    print(f"  EQUAL-WEIGHT composite IC@5s: Pearson={p_eq:+.4f} Spearman={s_eq:+.4f}  "
          f"({'beats best single component' if s_eq > best_single else 'does not beat best single component'} "
          f"[best single = {best_single:+.4f}])")

    # walk-forward: fit OLS weights on H1 only, score composite IC on H2 only
    h1 = grid < t_mid
    h2 = ~h1
    fwd_all = np.full_like(mid_grid, np.nan)
    fwd_all[:-5] = mid_grid[5:] - mid_grid[:-5]
    valid_all = ~np.isnan(fwd_all)

    X_all = np.column_stack(z_components)
    train_mask = h1 & valid_all
    test_mask = h2 & valid_all
    if train_mask.sum() > 10 and test_mask.sum() > 10:
        X_train, y_train = X_all[train_mask], fwd_all[train_mask]
        beta, *_ = np.linalg.lstsq(np.column_stack([X_train, np.ones(len(X_train))]), y_train, rcond=None)
        X_test = X_all[test_mask]
        pred_test = X_test @ beta[:-1] + beta[-1]
        y_test = fwd_all[test_mask]
        p_ols = pearson_corr(pred_test, y_test)
        s_ols = spearman_ic(pred_test, y_test)
        weights_str = ", ".join(f"{n}={b:+.3f}" for n, b in zip(components.keys(), beta[:-1]))
        print(f"  OLS-weighted composite (fit on H1 only: {weights_str}) "
              f"-> scored on H2: Pearson={p_ols:+.4f} Spearman={s_ols:+.4f}")
        # for comparison, equal-weight composite's OWN out-of-sample (H2-only) IC
        p_eq_h2, s_eq_h2, _ = ic_at_horizon(equal_weight[h2], mid_grid[h2], 5)
        print(f"  (for comparison, equal-weight composite scored on H2 alone: "
              f"Pearson={p_eq_h2:+.4f} Spearman={s_eq_h2:+.4f})")


def compute_vpin(d):
    is_lit = d["etypes"] == 4
    t = d["times"][is_lit]
    sz = d["sizes"][is_lit].astype(float)
    sign = -d["directions"][is_lit].astype(float)  # aggressor sign: +1 buy, -1 sell

    total_vol = sz.sum()
    V = total_vol / VPIN_N_BUCKETS

    bucket_times, bucket_imbalance = [], []
    buy_acc = sell_acc = vol_acc = 0.0
    for i in range(len(t)):
        if sign[i] > 0:
            buy_acc += sz[i]
        else:
            sell_acc += sz[i]
        vol_acc += sz[i]
        if vol_acc >= V:
            bucket_imbalance.append(abs(buy_acc - sell_acc))
            bucket_times.append(t[i])
            buy_acc = sell_acc = vol_acc = 0.0
    bucket_times = np.array(bucket_times)
    bucket_imbalance = np.array(bucket_imbalance)

    n = len(bucket_imbalance)
    vpin = np.full(n, np.nan)
    for i in range(n):
        lo = max(0, i - VPIN_ROLLING + 1)
        vpin[i] = bucket_imbalance[lo:i + 1].mean() / V
    return bucket_times, vpin, V, n


def vpin_analysis(ticker, d, mids, mids_before):
    print("[VPIN]")
    bucket_times, vpin, V, n_buckets = compute_vpin(d)
    print(f"  {n_buckets} volume buckets over the session (V={V:.0f} shares/bucket, "
          f"rolling window={VPIN_ROLLING} buckets) -- note: this is a deliberately finer "
          f"bucketing than Easley et al.'s 50-per-day convention, chosen to get a usable "
          f"sample size from a single session; treat n={n_buckets} as still modest for IC purposes.")

    # (i) does elevated VPIN predict subsequent |return|?
    valid_vpin = ~np.isnan(vpin)
    bt, vp = bucket_times[valid_vpin], vpin[valid_vpin]
    for h in [5.0, 30.0, 60.0]:
        query_t = bt + h
        ok = query_t <= SESSION_END_S
        idx = np.clip(np.searchsorted(d["times"], query_t[ok], side="right") - 1, 0, len(mids) - 1)
        idx0 = np.clip(np.searchsorted(d["times"], bt[ok], side="right") - 1, 0, len(mids) - 1)
        fwd_abs_ret = np.abs(mids[idx] - mids[idx0]) * 100.0  # cents
        p = pearson_corr(vp[ok], fwd_abs_ret)
        s = spearman_ic(vp[ok], fwd_abs_ret)
        print(f"  VPIN -> |forward return| @{h:.0f}s: Pearson={p:+.4f} Spearman={s:+.4f}  (n={ok.sum()})")

    # (ii) direct reconnection to phase 3: VPIN at fill time vs that fill's own markout
    is_lit = d["etypes"] == 4
    fill_times = d["times"][is_lit]
    fill_provider_sign = d["directions"][is_lit].astype(float)
    fill_mb = mids_before[is_lit]

    vpin_at_fill = np.full(len(fill_times), np.nan)
    idx_v = np.searchsorted(bt, fill_times, side="right") - 1
    ok_v = idx_v >= 0
    vpin_at_fill[ok_v] = vp[idx_v[ok_v]]

    horizon = 5.0
    query_t = fill_times + horizon
    ok = (query_t <= SESSION_END_S) & ~np.isnan(vpin_at_fill)
    idx_future = np.clip(np.searchsorted(d["times"], query_t[ok], side="right") - 1, 0, len(mids) - 1)
    future_mid = mids[idx_future]
    markout = fill_provider_sign[ok] * (future_mid - fill_mb[ok]) * 100.0

    vpin_ok = vpin_at_fill[ok]
    q1, q2, q3 = np.percentile(vpin_ok, [25, 50, 75])
    bucket = np.digitize(vpin_ok, [q1, q2, q3])
    means = [float(markout[bucket == b].mean()) if np.any(bucket == b) else float("nan") for b in range(4)]
    print(f"  fill markout @5s BY VPIN-at-fill-time quartile (Q1 calmest -> Q4 most toxic): "
          + "  ".join(f"Q{b+1}={m:+.3f}c" for b, m in enumerate(means)))
    populated = [m for m in means if not np.isnan(m)]
    monotone_worse = all(a >= b - 1e-9 for a, b in zip(populated, populated[1:]))
    print(f"  -> {'markout gets monotonically WORSE with higher VPIN (toxicity signal confirmed)' if monotone_worse else 'no clean monotonic relationship between VPIN and markout'}")


def analyze_alpha3(ticker: str):
    print(f"\n{'=' * 70}\n{ticker} -- alpha research, phase 7 (composite + VPIN)\n{'=' * 70}")
    d = stream_depth(ticker)
    mids = (d["ask_p1"] + d["bid_p1"]) / 2.0
    mids_before = np.empty_like(mids)
    mids_before[0] = mids[0]
    mids_before[1:] = mids[:-1]

    grid = np.arange(SESSION_START_S, SESSION_END_S, 1.0)
    idx_last = grid_index(d["times"], grid)
    mid_grid = mids[idx_last]
    bucket_idx = np.clip(np.searchsorted(grid, d["times"], side="right") - 1, 0, len(grid) - 1)
    t_mid = (SESSION_START_S + SESSION_END_S) / 2.0

    composite_analysis(ticker, d, grid, idx_last, bucket_idx, mid_grid, t_mid)
    vpin_analysis(ticker, d, mids, mids_before)


def main():
    for t in TICKERS:
        analyze_alpha3(t)


if __name__ == "__main__":
    sys.exit(main())
