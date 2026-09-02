#!/usr/bin/env python3
"""
Phase 8 (closing): the two gaps identified as still worth closing before
writing strategy code -- everything else from here on needs the actual
bot running to answer honestly, not more static analysis of the raw data.

  1. Confidence intervals on phase 3's markout numbers -- every P&L
     verdict (AAPL breakeven, AMZN negative by 30s, INTC/MSFT losing money
     throughout) was a point estimate with no standard error. Closes that.

  2. A data-derived inventory limit -- turns numbers already measured
     (median fill size, trade-sign persistence, sigma) into a concrete
     q_max suggestion, instead of leaving position sizing as an arbitrary
     constant nowhere in this analysis addresses.

Numpy-only, consistent with phases 1-7.
"""

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_dataset import SESSION_END_S, SESSION_START_S, TICKERS  # noqa: E402
from analyze_dataset_alpha2 import stream_depth  # noqa: E402
from analyze_dataset_signals import generalized_autocorr  # noqa: E402

MARKOUT_HORIZONS_S = [0.1, 0.5, 1.0, 5.0, 30.0]
RUN_LENGTH_SAFETY_MULTIPLIER = 3.0  # tunable -- see writeup; not derived from data
NEWEY_WEST_MAX_LAG = 20  # matches the largest lag phase 3's ACF work already
                          # characterized (ACF_LAGS_S), rather than picking a new,
                          # unrelated bandwidth for this correction


def newey_west_se(x: np.ndarray, max_lag: int) -> float:
    """HAC (Newey-West, Bartlett kernel) standard error of the sample mean.
    naive SE = std/sqrt(n) assumes i.i.d. draws -- phase 3 found fills are
    strongly autocorrelated in sign (0.7-0.9 at lag 1), and markout values
    inherit that autocorrelation (consecutive fills in a directional run
    share correlated outcomes). Ignoring that understates the true SE and
    overstates significance. This corrects for it."""
    n = len(x)
    xc = x - x.mean()
    gamma0 = float(np.sum(xc ** 2)) / n
    long_run_var = gamma0
    for k in range(1, min(max_lag, n - 1) + 1):
        w = 1.0 - k / (max_lag + 1)  # Bartlett kernel
        gamma_k = float(np.sum(xc[:-k] * xc[k:])) / n
        long_run_var += 2.0 * w * gamma_k
    long_run_var = max(long_run_var, 0.0)  # guard against numerical negative variance
    return float(np.sqrt(long_run_var / n))


def markout_with_ci(d, mids, mids_before):
    is_lit = d["etypes"] == 4
    t = d["times"][is_lit]
    trade_px = d["prices"][is_lit]
    mb = mids_before[is_lit]
    provider_sign = d["directions"][is_lit].astype(float)

    print("  horizon    mean P&L    SE(naive)  SE(HAC)   95% CI (HAC)            t-stat(HAC)  significant?")
    for h in MARKOUT_HORIZONS_S:
        query_t = t + h
        valid = query_t <= SESSION_END_S
        idx = np.searchsorted(d["times"], query_t[valid], side="right") - 1
        idx = np.clip(idx, 0, len(mids) - 1)
        future_mid = mids[idx]
        total_pnl = provider_sign[valid] * (future_mid - trade_px[valid]) * 100.0  # cents
        n = len(total_pnl)
        mean = total_pnl.mean()
        se_naive = total_pnl.std(ddof=1) / np.sqrt(n)
        se_hac = newey_west_se(total_pnl, NEWEY_WEST_MAX_LAG)
        ci_lo, ci_hi = mean - 1.96 * se_hac, mean + 1.96 * se_hac
        t_stat = mean / se_hac if se_hac > 0 else float("nan")
        sig = "yes (|t|>1.96)" if abs(t_stat) > 1.96 else "NOT significant"
        sign_note = "profitable" if ci_lo > 0 else "LOSS" if ci_hi < 0 else "CI spans zero"
        print(f"  {h:>6.1f}s  {mean:>+8.3f}c  {se_naive:.3f}c    {se_hac:.3f}c   "
              f"[{ci_lo:+.3f}, {ci_hi:+.3f}]c   {t_stat:>+7.2f}      {sig}  -> {sign_note}")


def inventory_limit(ticker, d, sigma_1s, median_lit_fill_size):
    is_lit = d["etypes"] == 4
    aggressor_sign = -d["directions"][is_lit].astype(float)
    lags = [1, 5, 20]
    acs = [generalized_autocorr(aggressor_sign, lag) for lag in lags]

    # fit ac(lag) ~ rho^lag via log-linear regression on the 3 known points
    # (all three were positive in every name per phase 3 -- guard anyway)
    if all(a > 0 for a in acs):
        x = np.array(lags, dtype=float)
        y = np.log(np.array(acs))
        slope, intercept = np.polyfit(x, y, 1)
        rho = float(np.exp(slope))
        expected_run_length = 1.0 / (1.0 - rho) if rho < 1.0 else float("inf")
    else:
        rho = float("nan")
        expected_run_length = float("nan")

    q_max = RUN_LENGTH_SAFETY_MULTIPLIER * expected_run_length * median_lit_fill_size
    # sigma_1s is already in $/sqrt(s) (mids array is dollar-denominated) --
    # q_max (shares) * sigma_1s ($/share per sqrt(s)) * sqrt(60s) = total $ risk
    dollar_risk_60s = q_max * sigma_1s * np.sqrt(60.0)

    print(f"  trade-sign autocorr used for this fit: lag1={acs[0]:+.3f} lag5={acs[1]:+.3f} lag20={acs[2]:+.3f}")
    print(f"  fitted persistence rho={rho:.4f}  ->  expected same-direction run length ~= {expected_run_length:.1f} fills")
    print(f"  median lit-fill size = {median_lit_fill_size:.0f} shares")
    print(f"  suggested q_max = {RUN_LENGTH_SAFETY_MULTIPLIER:.0f}x expected run length x median fill size "
          f"= {q_max:,.0f} shares")
    print(f"  -> holding q_max shares, a {60}s adverse move carries a 1-sigma TOTAL P&L swing of "
          f"~${dollar_risk_60s:,.2f} (sigma={sigma_1s:.5f} $/sqrt(s))")
    print(f"  NOTE: the {RUN_LENGTH_SAFETY_MULTIPLIER:.0f}x safety multiplier and the 60s risk-evaluation "
          f"window are free choices, not derived from data -- everything else in this q_max is measured.")


def analyze_riskparams(ticker: str):
    print(f"\n{'=' * 70}\n{ticker} -- risk parameters (closing analysis)\n{'=' * 70}")
    d = stream_depth(ticker)
    mids = (d["ask_p1"] + d["bid_p1"]) / 2.0
    mids_before = np.empty_like(mids)
    mids_before[0] = mids[0]
    mids_before[1:] = mids[:-1]

    grid = np.arange(SESSION_START_S, SESSION_END_S, 1.0)
    idx = np.clip(np.searchsorted(d["times"], grid, side="right") - 1, 0, len(mids) - 1)
    sigma_1s = float(np.std(np.diff(mids[idx])))

    is_lit = d["etypes"] == 4
    median_lit_fill_size = float(np.median(d["sizes"][is_lit]))

    print("[1] Markout with confidence intervals]")
    markout_with_ci(d, mids, mids_before)

    print("[2] Data-derived inventory limit]")
    inventory_limit(ticker, d, sigma_1s, median_lit_fill_size)


def main():
    for t in TICKERS:
        analyze_riskparams(t)


if __name__ == "__main__":
    sys.exit(main())
