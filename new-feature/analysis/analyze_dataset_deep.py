#!/usr/bin/env python3
"""
Deep-dive pass, phase 2 of the pre-calibration diagnostics. Builds on
analyze_dataset.py (imported, not duplicated) and answers seven questions
phase 1 didn't:

  1. Quoted spread distribution & time-at-minimum-tick -- makes the
     phase-1 INTC/MSFT "72% of fills at zero depth" finding a directly
     measured fact instead of an inference.
  2. Lit-only vs pooled k -- the strategy this feeds rests DISPLAYED GTC
     limit orders; it can only ever be filled the way a type-4 (visible)
     execution represents. Pooling in type-5 (hidden/dark, often
     midpoint-priced) fills into the k regression overstates the arrival
     rate a displayed quote should expect. Phase 1 pooled them; this
     checks how much that matters.
  3. Bid-side vs ask-side k -- is a single symmetric k defensible, or is
     the book's liquidity-taking asymmetric?
  4. Session-half parameter stability -- refit sigma and k independently
     on the first half and second half of the session. This is the actual
     answer to "how much would this calibration drift if I used it on a
     different hour," not just an intraday-seasonality plot.
  5. Fat tails -- excess kurtosis of 1s price differences, plus the
     empirical vs Gaussian-predicted rate of >3-sigma moves. A-S assumes
     Gaussian increments; this tests it directly instead of assuming it.
  6. Volatility clustering (ARCH effect) -- autocorrelation of squared 1s
     price differences. Directly tests "constant sigma," which phase 1's
     lag-1 autocorrelation of raw differences does not.
  7. Lit vs hidden fill composition -- what fraction of "fills" are even
     visible to a displayed-quote strategy, and do the two populations
     differ in size.

Same no-pandas/scipy, numpy-only policy as phase 1.
"""

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_dataset import (  # noqa: E402
    SESSION_END_S,
    SESSION_START_S,
    TICKERS,
    lag1_autocorr,
    realized_vol_arithmetic,
    stream_ticker,
    tick_size,
)

DEPTH_BIN_CENTS = 1.0
MAX_DEPTH_CENTS = 20.0


def time_weighted_mean_and_frac_below(times, values, t_end, threshold):
    """Weight each (time-invariant-until-next-event) value by how long it
    actually persisted, not by how many messages happened to arrive during
    it -- a burst of 50 messages in 10ms shouldn't count 50x more than one
    message that held for 2 seconds."""
    durations = np.empty_like(times)
    durations[:-1] = np.diff(times)
    durations[-1] = max(t_end - times[-1], 0.0)
    durations = np.clip(durations, 0.0, None)
    total = durations.sum()
    if total <= 0:
        return float("nan"), float("nan")
    weighted_mean = float(np.sum(values * durations) / total)
    frac_below = float(np.sum(durations[values <= threshold]) / total)
    return weighted_mean, frac_below


def fit_lambda_delta(deltas_cents: np.ndarray, session_seconds: float):
    """Shared exponential-decay fit used by every k variant below (pooled,
    lit-only, bid/ask split, session-half split). delta=0 bin excluded --
    see analyze_dataset.py's depth_from_mid_and_intensity for why."""
    edges = np.arange(0.0, MAX_DEPTH_CENTS + DEPTH_BIN_CENTS, DEPTH_BIN_CENTS)
    counts, _ = np.histogram(deltas_cents, bins=edges)
    bin_centers = (edges[:-1] + edges[1:]) / 2.0
    rates = counts / session_seconds

    fit_mask = counts > 0
    fit_mask[0] = False
    if fit_mask.sum() < 2:
        return float("nan"), float("nan"), float("nan"), int(fit_mask.sum())

    x = bin_centers[fit_mask]
    y = np.log(rates[fit_mask])
    A_mat = np.vstack([x, np.ones_like(x)]).T
    slope, intercept = np.linalg.lstsq(A_mat, y, rcond=None)[0]
    y_pred = A_mat @ np.array([slope, intercept])
    ss_res = np.sum((y - y_pred) ** 2)
    ss_tot = np.sum((y - y.mean()) ** 2)
    r2 = 1 - ss_res / ss_tot if ss_tot > 0 else float("nan")
    return -slope, intercept, r2, int(fit_mask.sum())


def excess_kurtosis(x: np.ndarray) -> float:
    mu, sigma = x.mean(), x.std()
    if sigma == 0:
        return float("nan")
    return float(np.mean(((x - mu) / sigma) ** 4) - 3.0)


def tail_ratio(x: np.ndarray, z: float = 3.0) -> float:
    """Empirical P(|z-score| > z) vs. the Gaussian prediction. Gaussian
    z=3 -> ~0.27%; a ratio >> 1 means fatter-than-Gaussian tails."""
    mu, sigma = x.mean(), x.std()
    if sigma == 0:
        return float("nan")
    zscores = (x - mu) / sigma
    empirical = float(np.mean(np.abs(zscores) > z))
    from math import erfc, sqrt

    gaussian = erfc(z / sqrt(2))  # two-sided tail prob under N(0,1)
    return empirical / gaussian if gaussian > 0 else float("nan")


def analyze_deep(ticker: str):
    print(f"\n{'=' * 70}\n{ticker} -- deep dive\n{'=' * 70}")
    times, etypes, sizes, prices, directions, best_asks, best_bids = stream_ticker(
        ticker
    )
    mids = (best_asks + best_bids) / 2.0
    mids_before = np.empty_like(mids)
    mids_before[0] = mids[0]
    mids_before[1:] = mids[:-1]
    session_seconds = SESSION_END_S - SESSION_START_S
    is_lit_fill = etypes == 4
    is_hidden_fill = etypes == 5
    is_fill = is_lit_fill | is_hidden_fill

    # ---- 1. quoted spread distribution, time-weighted ----
    spread_cents = (best_asks - best_bids) * 100.0
    ts_lit = tick_size(prices[etypes == 1])
    min_tick_cents = ts_lit * 100.0
    mean_spread, frac_time_at_min_tick = time_weighted_mean_and_frac_below(
        times, spread_cents, SESSION_END_S, min_tick_cents + 1e-6
    )
    median_spread = float(np.median(spread_cents))
    print(f"quoted spread: time-weighted mean={mean_spread:.3f}c  "
          f"median(unweighted)={median_spread:.2f}c  "
          f"time at minimum tick ({min_tick_cents:.1f}c): {frac_time_at_min_tick:.1%}")

    # ---- 2. lit-only vs pooled k ----
    deltas_all = np.abs(prices[is_fill] - mids_before[is_fill]) * 100.0
    deltas_lit = np.abs(prices[is_lit_fill] - mids_before[is_lit_fill]) * 100.0
    k_pooled, lnA_pooled, r2_pooled, nb_pooled = fit_lambda_delta(deltas_all, session_seconds)
    k_lit, lnA_lit, r2_lit, nb_lit = fit_lambda_delta(deltas_lit, session_seconds)
    print(f"k (pooled, type 4+5): k={k_pooled:.4f}/c  R2={r2_pooled:.3f}  bins={nb_pooled}")
    print(f"k (lit-only, type 4): k={k_lit:.4f}/c  R2={r2_lit:.3f}  bins={nb_lit}"
          f"  <- the one the strategy should actually use")
    if not (np.isnan(k_pooled) or np.isnan(k_lit)) and k_lit != 0:
        print(f"  pooled/lit k ratio: {k_pooled / k_lit:.2f}x "
              f"({'pooling meaningfully changes the estimate' if abs(k_pooled / k_lit - 1) > 0.1 else 'pooling barely matters here'})")

    # ---- 3. bid-side vs ask-side k, lit-only ----
    # LOBSTER 'direction' on an execution message: +1 = a resting BUY (bid)
    # order was executed (an aggressive sell hit the bid); -1 = a resting
    # SELL (ask) order was executed (an aggressive buy hit the ask). So
    # direction==+1 fills happened at/near the bid side, -1 at/near the ask.
    lit_dir = directions[is_lit_fill]
    lit_deltas = deltas_lit
    k_bid, _, r2_bid, nb_bid = fit_lambda_delta(lit_deltas[lit_dir == 1], session_seconds)
    k_ask, _, r2_ask, nb_ask = fit_lambda_delta(lit_deltas[lit_dir == -1], session_seconds)
    print(f"k split by side (lit-only): bid-side k={k_bid:.4f}/c (R2={r2_bid:.3f}, bins={nb_bid})  "
          f"ask-side k={k_ask:.4f}/c (R2={r2_ask:.3f}, bins={nb_ask})")

    # ---- 4. session-half stability: sigma and lit-only k, first vs second half ----
    t_mid = (SESSION_START_S + SESSION_END_S) / 2.0
    first_half = times < t_mid
    second_half = ~first_half

    # realized_vol_arithmetic grids on the module-level [SESSION_START_S,
    # SESSION_END_S) constants; for a half-session estimate we need it
    # gridded on that half only, so inline the same computation here with
    # the correct bounds instead of reusing that function directly:
    def half_sigma(t0, t1):
        mask = (times >= t0) & (times < t1)
        grid = np.arange(t0, t1, 1.0)
        idx = np.searchsorted(times[mask], grid, side="right") - 1
        idx = np.clip(idx, 0, mask.sum() - 1)
        sub_mids = mids[mask]
        if len(sub_mids) < 3:
            return float("nan")
        gm = sub_mids[idx]
        return float(np.std(np.diff(gm)))

    sigma_h1 = half_sigma(SESSION_START_S, t_mid)
    sigma_h2 = half_sigma(t_mid, SESSION_END_S)
    ratio = sigma_h2 / sigma_h1 if sigma_h1 else float("nan")
    print(f"sigma stability: first-half={sigma_h1:.5f}  second-half={sigma_h2:.5f}  "
          f"ratio={ratio:.2f}x")

    deltas_lit_h1 = np.abs(
        prices[is_lit_fill & first_half] - mids_before[is_lit_fill & first_half]
    ) * 100.0
    deltas_lit_h2 = np.abs(
        prices[is_lit_fill & second_half] - mids_before[is_lit_fill & second_half]
    ) * 100.0
    k_h1, _, r2_h1, nb_h1 = fit_lambda_delta(deltas_lit_h1, session_seconds / 2.0)
    k_h2, _, r2_h2, nb_h2 = fit_lambda_delta(deltas_lit_h2, session_seconds / 2.0)
    print(f"k stability (lit-only): first-half={k_h1:.4f}/c (R2={r2_h1:.3f})  "
          f"second-half={k_h2:.4f}/c (R2={r2_h2:.3f})")

    # ---- 5. fat tails ----
    _, diffs_1s = realized_vol_arithmetic(times, mids, dt=1.0)
    ek = excess_kurtosis(diffs_1s)
    tr = tail_ratio(diffs_1s, z=3.0)
    tail_flag = "Gaussian-consistent" if 0.5 <= tr <= 2.0 else "FAT TAILS vs Gaussian"
    print(f"1s price-diff distribution: excess kurtosis={ek:+.2f} "
          f"(0=Gaussian, >0=fat-tailed)  P(|z|>3)/Gaussian-P(|z|>3)={tr:.2f}x -> {tail_flag}")

    # ---- 6. volatility clustering (ARCH effect) ----
    sq = diffs_1s ** 2
    arch_ac1 = lag1_autocorr(sq)
    n = len(sq)
    band = 1.96 / np.sqrt(n)
    arch_flag = "VOLATILITY CLUSTERING present" if abs(arch_ac1) > band else "no detectable clustering"
    print(f"ARCH effect: lag-1 autocorr of squared 1s diffs = {arch_ac1:+.4f} "
          f"(band +/-{band:.4f}) -> {arch_flag}")

    # ---- 7. lit vs hidden fill composition ----
    n_lit, n_hidden = int(is_lit_fill.sum()), int(is_hidden_fill.sum())
    mean_size_lit = float(np.mean(sizes[is_lit_fill])) if n_lit else float("nan")
    mean_size_hidden = float(np.mean(sizes[is_hidden_fill])) if n_hidden else float("nan")
    print(f"fill composition: {n_lit:,} lit (visible) + {n_hidden:,} hidden "
          f"= {n_lit + n_hidden:,} total ({n_hidden / (n_lit + n_hidden):.1%} hidden)")
    print(f"  mean size: lit={mean_size_lit:.0f} shares  hidden={mean_size_hidden:.0f} shares")

    return {
        "ticker": ticker,
        "mean_spread_cents": mean_spread,
        "frac_time_at_min_tick": frac_time_at_min_tick,
        "k_pooled": k_pooled,
        "k_lit": k_lit,
        "k_bid": k_bid,
        "k_ask": k_ask,
        "sigma_h1": sigma_h1,
        "sigma_h2": sigma_h2,
        "sigma_ratio": ratio,
        "k_h1": k_h1,
        "k_h2": k_h2,
        "excess_kurtosis": ek,
        "tail_ratio": tr,
        "arch_ac1": arch_ac1,
        "arch_band": band,
        "frac_hidden": n_hidden / (n_lit + n_hidden) if (n_lit + n_hidden) else float("nan"),
    }


def main():
    results = [analyze_deep(t) for t in TICKERS]

    print(f"\n{'=' * 70}\nDEEP-DIVE CROSS-SECTIONAL SUMMARY\n{'=' * 70}")
    header = (f"{'ticker':6} {'spread$c':>9} {'%@mintick':>10} {'k_pool':>7} {'k_lit':>7} "
              f"{'k_bid':>7} {'k_ask':>7} {'sig_ratio':>9} {'kurt':>7} {'tailR':>7} {'archAC1':>8} {'%hidden':>8}")
    print(header)
    for r in results:
        print(f"{r['ticker']:6} {r['mean_spread_cents']:9.3f} {r['frac_time_at_min_tick']:10.1%} "
              f"{r['k_pooled']:7.4f} {r['k_lit']:7.4f} {r['k_bid']:7.4f} {r['k_ask']:7.4f} "
              f"{r['sigma_ratio']:9.2f} {r['excess_kurtosis']:7.2f} {r['tail_ratio']:7.2f} "
              f"{r['arch_ac1']:+8.4f} {r['frac_hidden']:8.1%}")


if __name__ == "__main__":
    sys.exit(main())
