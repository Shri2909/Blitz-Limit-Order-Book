#!/usr/bin/env python3
"""
Phase 4: the last two prerequisite checks before writing any strategy code.

  1. Toxic-flow detection -- does a LARGER fill predict WORSE subsequent
     markout? If size itself carries information about how adverse a fill
     is going to turn out, that's a direct, actionable signal: shrink
     quoted size (or widen) after a large print, rather than treating
     every fill of every size as equally risky.

  2. Walk-forward validation of sigma and k -- phase 2 fit sigma/k
     separately on each session half and reported how much they drifted.
     That's a diagnostic, not a validation. The actual test: fit sigma and
     k on the FIRST half only, then check how well that first-half fit
     predicts the SECOND half's actual behavior it never saw. This is the
     standard "would this have generalized" check, and it's the honest way
     to answer "how do you know this isn't overfit to the whole session."

Reuses stream_full (order_id + L1 sizes) from phase 3 and fit_lambda_delta
from phase 2 rather than re-deriving either. Numpy-only, same as phases 1-3.
"""

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_dataset import SESSION_END_S, SESSION_START_S, TICKERS  # noqa: E402
from analyze_dataset_deep import DEPTH_BIN_CENTS, MAX_DEPTH_CENTS, fit_lambda_delta  # noqa: E402
from analyze_dataset_signals import stream_full  # noqa: E402


# ---------- 1. toxic-flow detection: fill size vs. markout ----------

def toxic_flow_check(d, mids, mids_before, horizons=(1.0, 5.0)):
    is_lit = d["etypes"] == 4
    t = d["times"][is_lit]
    trade_px = d["prices"][is_lit]
    sz = d["sizes"][is_lit].astype(float)
    provider_sign = d["directions"][is_lit].astype(float)
    mb = mids_before[is_lit]

    for h in horizons:
        query_t = t + h
        valid = query_t <= SESSION_END_S
        idx = np.clip(np.searchsorted(d["times"], query_t[valid], side="right") - 1, 0, len(mids) - 1)
        future_mid = mids[idx]
        markout = provider_sign[valid] * (future_mid - mb[valid]) * 100.0  # cents, provider's side
        sizes_v = sz[valid]

        x = sizes_v - sizes_v.mean()
        y = markout - markout.mean()
        denom = np.sqrt((x @ x) * (y @ y))
        corr = float((x @ y) / denom) if denom > 0 else float("nan")

        q1, q2, q3 = np.percentile(sizes_v, [25, 50, 75])
        bucket = np.digitize(sizes_v, [q1, q2, q3])  # 0=smallest quartile .. 3=largest
        bucket_means = [float(markout[bucket == b].mean()) if np.any(bucket == b) else float("nan")
                         for b in range(4)]
        print(f"  @{h:.0f}s: corr(size, markout)={corr:+.4f}   "
              f"markout by size quartile (Q1 smallest -> Q4 largest): "
              + "  ".join(f"Q{b+1}={m:+.3f}c" for b, m in enumerate(bucket_means)))
    return


# ---------- 2. walk-forward validation of sigma and k ----------

def half_grid_diffs(times, mids, t0, t1, dt=1.0):
    mask = (times >= t0) & (times < t1)
    grid = np.arange(t0, t1, dt)
    idx = np.searchsorted(times[mask], grid, side="right") - 1
    idx = np.clip(idx, 0, mask.sum() - 1)
    sub_mids = mids[mask]
    if len(sub_mids) < 3:
        return np.array([])
    return np.diff(sub_mids[idx])


def walkforward_sigma(times, mids, t_mid):
    diffs_h1 = half_grid_diffs(times, mids, SESSION_START_S, t_mid)
    diffs_h2 = half_grid_diffs(times, mids, t_mid, SESSION_END_S)
    sigma_h1 = float(np.std(diffs_h1))
    sigma_h2_actual = float(np.std(diffs_h2))
    z = diffs_h2 / sigma_h1  # standardize UNSEEN h2 data by the h1-only estimate
    std_z = float(np.std(z))
    pct_err = (sigma_h2_actual - sigma_h1) / sigma_h1 * 100.0
    print(f"  sigma: fit on H1 only = {sigma_h1:.5f}  ->  actual H2 = {sigma_h2_actual:.5f}  "
          f"(forecast error {pct_err:+.1f}%)")
    print(f"  H2 returns standardized by H1-only sigma: std(z) = {std_z:.3f} "
          f"(1.00 = perfect out-of-sample forecast; {'H2 was calmer than H1 predicted' if std_z < 0.9 else 'H2 was more volatile than H1 predicted' if std_z > 1.1 else 'forecast held up reasonably'})")
    return sigma_h1, sigma_h2_actual, std_z


def walkforward_k(d, mids_before, t_mid, session_half_seconds):
    is_lit = d["etypes"] == 4
    times = d["times"]
    first_half = times < t_mid
    second_half = ~first_half

    deltas_h1 = np.abs(d["prices"][is_lit & first_half] - mids_before[is_lit & first_half]) * 100.0
    deltas_h2 = np.abs(d["prices"][is_lit & second_half] - mids_before[is_lit & second_half]) * 100.0

    k_h1, lnA_h1, r2_h1_insample, nb_h1 = fit_lambda_delta(deltas_h1, session_half_seconds)
    if np.isnan(k_h1):
        print("  k: first-half fit not identified (insufficient bins) -- walk-forward not applicable")
        return float("nan"), float("nan")

    edges = np.arange(0.0, MAX_DEPTH_CENTS + DEPTH_BIN_CENTS, DEPTH_BIN_CENTS)
    counts_h2, _ = np.histogram(deltas_h2, bins=edges)
    bin_centers = (edges[:-1] + edges[1:]) / 2.0
    actual_rate_h2 = counts_h2 / session_half_seconds

    # exclude delta=0 bin (index 0) by INDEX, matching phase 1/2's fit_mask[0]=False --
    # bin_centers is [0.5, 1.5, ..., 19.5], every entry > 0, so "bin_centers > 0" was a
    # no-op that silently included the delta=0 bin in the out-of-sample scoring below.
    mask = counts_h2 > 0
    mask[0] = False
    if mask.sum() < 2:
        print("  k: second-half has insufficient bins to score the forecast against")
        return k_h1, float("nan")

    log_actual = np.log(actual_rate_h2[mask])
    log_pred = np.log(np.exp(lnA_h1 - k_h1 * bin_centers[mask]))
    resid = log_actual - log_pred
    ss_res = np.sum(resid ** 2)
    ss_tot = np.sum((log_actual - log_actual.mean()) ** 2)
    oos_r2 = 1 - ss_res / ss_tot if ss_tot > 0 else float("nan")
    mean_abs_pct_err = float(np.mean(np.abs(np.exp(log_actual) - np.exp(log_pred)) / np.exp(log_actual))) * 100.0

    print(f"  k: fit on H1 only = {k_h1:.4f}/c (in-sample R2={r2_h1_insample:.3f}, {nb_h1} bins)  "
          f"-> scored against ACTUAL H2 histogram (never seen by the fit):")
    print(f"     out-of-sample R2 = {oos_r2:.3f}   mean abs %% error on fill rate = {mean_abs_pct_err:.1f}%%   "
          f"({'holds up reasonably out-of-sample' if oos_r2 > 0.3 else 'DOES NOT generalize well out-of-sample'})")
    return k_h1, oos_r2


def analyze_validation(ticker: str):
    print(f"\n{'=' * 70}\n{ticker} -- validation (toxic flow + walk-forward)\n{'=' * 70}")
    d = stream_full(ticker)
    mids = (d["ask_p"] + d["bid_p"]) / 2.0
    mids_before = np.empty_like(mids)
    mids_before[0] = mids[0]
    mids_before[1:] = mids[:-1]
    t_mid = (SESSION_START_S + SESSION_END_S) / 2.0
    session_half_seconds = (SESSION_END_S - SESSION_START_S) / 2.0

    print("[1] toxic-flow check (fill size vs. subsequent markout):")
    toxic_flow_check(d, mids, mids_before)

    print("[2] walk-forward validation:")
    walkforward_sigma(d["times"], mids, t_mid)
    walkforward_k(d, mids_before, t_mid, session_half_seconds)


def main():
    for t in TICKERS:
        analyze_validation(t)


if __name__ == "__main__":
    sys.exit(main())
