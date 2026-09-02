#!/usr/bin/env python3
"""
Pre-calibration diagnostic pass over the LOBSTER real-data panel
(new-feature/datasets/) -- checks whether the Avellaneda-Stoikov model's
assumptions hold before any sigma/k estimate from this data is trusted.

Five checks, per ticker:
  1. Tick size          -- is price truly continuous, or does it need
                            snapping to a grid before quoting?
  2. Realized volatility -- sigma, arithmetic (price-unit) not log-return,
                            to match dS_t = sigma dW_t.
  3. Return autocorrelation -- is the mid-price series close enough to a
                            random walk for the no-drift assumption to be
                            defensible?
  4. Inter-arrival times of fills -- coefficient of variation vs. 1.0
                            (exponential/Poisson) as a clustering check.
  5. Fill intensity vs. depth-from-mid -- the lambda(delta) = A*exp(-k*delta)
                            regression that k actually comes from, plus the
                            raw depth distribution the regression is fit on
                            (most real fills cluster at delta=0 -- reported
                            explicitly, not smoothed over).

No pandas/scipy dependency -- this project's Python tooling (charts/) is
matplotlib-only; this stays numpy-only and streams both CSVs in one pass
per ticker rather than materializing the full 40-column orderbook matrix.
"""

import csv
import sys
from pathlib import Path

import numpy as np

DATASETS_DIR = Path(__file__).resolve().parent.parent / "datasets"
TICKERS = ["AAPL", "AMZN", "GOOG", "INTC", "MSFT"]
DEPTH_BIN_CENTS = 1.0  # bucket width for the delta-from-mid histogram, in cents
MAX_DEPTH_CENTS = 20.0  # ignore fills further than this from mid for the k-fit
SESSION_START_S = 34200.0  # 09:30:00
SESSION_END_S = 57600.0  # 16:00:00


def stream_ticker(ticker: str):
    """One pass over the line-aligned message/orderbook CSVs. Returns
    numpy arrays: time, event_type, size, price (USD), direction,
    best_ask (USD), best_bid (USD)."""
    msg_path = DATASETS_DIR / f"{ticker}_2012-06-21_message_10.csv"
    ob_path = DATASETS_DIR / f"{ticker}_2012-06-21_orderbook_10.csv"

    times, etypes, sizes, prices, directions = [], [], [], [], []
    best_asks, best_bids = [], []

    with open(msg_path, newline="") as mf, open(ob_path, newline="") as of:
        mreader = csv.reader(mf)
        oreader = csv.reader(of)
        for mrow, orow in zip(mreader, oreader):
            times.append(float(mrow[0]))
            etypes.append(int(mrow[1]))
            sizes.append(int(mrow[3]))
            prices.append(int(mrow[4]) / 1e4)
            directions.append(int(mrow[5]))
            best_asks.append(int(orow[0]) / 1e4)
            best_bids.append(int(orow[2]) / 1e4)

    return (
        np.array(times),
        np.array(etypes),
        np.array(sizes),
        np.array(prices),
        np.array(directions),
        np.array(best_asks),
        np.array(best_bids),
    )


def tick_size(prices: np.ndarray) -> float:
    uniq = np.unique(prices)
    diffs = np.diff(uniq)
    diffs = diffs[diffs > 1e-9]
    return float(np.min(diffs)) if len(diffs) else float("nan")


def realized_vol_arithmetic(times: np.ndarray, mids: np.ndarray, dt: float = 1.0):
    """Sigma in dollars / sqrt(second), from a fixed dt-second grid of the
    mid-price series (last value carried forward -- an as-of resample),
    matching dS_t = sigma dW_t (arithmetic BM), not log-return vol."""
    grid = np.arange(SESSION_START_S, SESSION_END_S, dt)
    idx = np.searchsorted(times, grid, side="right") - 1
    idx = np.clip(idx, 0, len(mids) - 1)
    grid_mids = mids[idx]
    diffs = np.diff(grid_mids)
    sigma = float(np.std(diffs)) / np.sqrt(dt)
    return sigma, diffs


def lag1_autocorr(x: np.ndarray) -> float:
    x0, x1 = x[:-1], x[1:]
    x0 = x0 - x0.mean()
    x1 = x1 - x1.mean()
    denom = np.sqrt((x0 @ x0) * (x1 @ x1))
    return float((x0 @ x1) / denom) if denom > 0 else float("nan")


def fill_inter_arrival_stats(fill_times: np.ndarray):
    iat = np.diff(fill_times)
    iat = iat[iat > 0]
    mean_iat = float(np.mean(iat))
    std_iat = float(np.std(iat))
    cv = std_iat / mean_iat if mean_iat > 0 else float("nan")
    frac_under_10ms = float(np.mean(iat < 0.010))
    return mean_iat, std_iat, cv, frac_under_10ms, len(iat)


def depth_from_mid_and_intensity(
    times, etypes, prices, mids_before, session_seconds
):
    """delta = |exec_price - mid immediately before the execution|, in
    cents. Returns (deltas_cents, bin_edges, rates_per_sec, k, lnA, r2,
    frac_at_zero)."""
    is_fill = (etypes == 4) | (etypes == 5)
    deltas = np.abs(prices[is_fill] - mids_before[is_fill]) * 100.0  # -> cents

    frac_at_zero = float(np.mean(deltas < 0.5))  # within half a cent of mid

    edges = np.arange(0.0, MAX_DEPTH_CENTS + DEPTH_BIN_CENTS, DEPTH_BIN_CENTS)
    counts, _ = np.histogram(deltas, bins=edges)
    bin_centers = (edges[:-1] + edges[1:]) / 2.0
    rates = counts / session_seconds  # fills/sec at that depth bucket

    # Fit ln(rate) = ln(A) - k*delta by OLS on bins with at least 1 fill,
    # skipping the delta=0 bucket -- lambda(delta)=A*exp(-k*delta) describes
    # the decay away from the touch, and the delta=0 bucket (executions
    # essentially AT the prevailing mid/touch) is exactly the regime the
    # asymptotic closed-form spread formula is built to extrapolate FROM,
    # not a point on its own decay curve -- including it here would fit the
    # line to its own boundary condition.
    nonzero = counts > 0
    fit_mask = nonzero.copy()
    fit_mask[0] = False
    if fit_mask.sum() >= 2:
        x = bin_centers[fit_mask]
        y = np.log(rates[fit_mask])
        A_mat = np.vstack([x, np.ones_like(x)]).T
        slope, intercept = np.linalg.lstsq(A_mat, y, rcond=None)[0]
        k = -slope
        lnA = intercept
        y_pred = A_mat @ np.array([slope, intercept])
        ss_res = np.sum((y - y_pred) ** 2)
        ss_tot = np.sum((y - y.mean()) ** 2)
        r2 = 1 - ss_res / ss_tot if ss_tot > 0 else float("nan")
    else:
        k = lnA = r2 = float("nan")

    return deltas, edges, rates, counts, k, lnA, r2, frac_at_zero


def intraday_seasonality(times, mids, fill_times, n_buckets=13):
    edges = np.linspace(SESSION_START_S, SESSION_END_S, n_buckets + 1)
    rows = []
    for i in range(n_buckets):
        t0, t1 = edges[i], edges[i + 1]
        mask = (times >= t0) & (times < t1)
        sub_mids = mids[mask]
        vol = float(np.std(np.diff(sub_mids))) if len(sub_mids) > 2 else float("nan")
        fcount = int(np.sum((fill_times >= t0) & (fill_times < t1)))
        rows.append((t0, t1, vol, fcount))
    return rows


def analyze(ticker: str):
    print(f"\n{'=' * 70}\n{ticker}\n{'=' * 70}")
    times, etypes, sizes, prices, directions, best_asks, best_bids = stream_ticker(
        ticker
    )
    mids = (best_asks + best_bids) / 2.0
    session_seconds = SESSION_END_S - SESSION_START_S

    # sanity: monotonic time, no sentinel-price contamination in mids
    assert np.all(np.diff(times) >= 0), f"{ticker}: message times not sorted"
    bad = np.sum((mids <= 0) | (mids > 100000))
    print(f"rows: {len(times):,}  |  bad/sentinel mid rows: {bad}")

    # 1. tick size -- computed separately for lit new limit orders (type 1,
    #    always on the displayed grid) vs. all prices (which also includes
    #    executions against HIDDEN orders, type 5, frequently priced at the
    #    bid-ask midpoint -- i.e. a real, expected source of sub-penny
    #    prices, not a data-quality problem). Reporting both rather than
    #    quoting one number avoids mistaking "this instrument trades in
    #    half-cent increments" for what's actually "10-15% of fills are
    #    hidden midpoint executions."
    ts_lit = tick_size(prices[etypes == 1])
    ts_all = tick_size(prices)
    print(f"tick size (lit new-order prices, type 1 only): ${ts_lit:.4f}")
    print(f"tick size (all prices incl. hidden execs):      ${ts_all:.4f}"
          f"{'  <- sub-penny, consistent with midpoint-priced hidden fills' if ts_all < ts_lit else ''}")
    ts = ts_lit

    # 2. realized volatility (arithmetic, $/sqrt(s)), at two sampling rates
    #    as a stability cross-check on the estimate itself
    sigma_1s, diffs_1s = realized_vol_arithmetic(times, mids, dt=1.0)
    sigma_5s, diffs_5s = realized_vol_arithmetic(times, mids, dt=5.0)
    print(f"sigma (1s sampling):  ${sigma_1s:.5f} / sqrt(s)")
    print(f"sigma (5s sampling):  ${sigma_5s:.5f} / sqrt(s)  "
          f"(ratio to 1s: {sigma_5s / sigma_1s:.2f}, "
          f"1.00 expected under pure Brownian scaling)")

    # 3. autocorrelation of 1s price differences
    ac1 = lag1_autocorr(diffs_1s)
    n = len(diffs_1s)
    band = 1.96 / np.sqrt(n)
    flag = "WITHIN +/-1.96/sqrt(N) noise band" if abs(ac1) <= band else "OUTSIDE noise band"
    print(f"lag-1 autocorr (1s price diffs): {ac1:+.4f}  "
          f"(band +/-{band:.4f}, N={n:,}) -> {flag}")

    # 4. fill inter-arrival times
    is_fill = (etypes == 4) | (etypes == 5)
    fill_times = times[is_fill]
    mean_iat, std_iat, cv, frac_under_10ms, n_iat = fill_inter_arrival_stats(fill_times)
    poisson_flag = "consistent with Poisson (CV~1)" if 0.85 <= cv <= 1.15 else (
        "OVER-dispersed / clustered (CV>1)" if cv > 1.15 else "UNDER-dispersed / too regular (CV<1)"
    )
    print(f"fill inter-arrival: n={n_iat:,}  mean={mean_iat*1000:.1f}ms  "
          f"std={std_iat*1000:.1f}ms  CV={cv:.2f} -> {poisson_flag}")
    print(f"  fraction of inter-arrivals < 10ms: {frac_under_10ms:.1%} "
          f"(excess mass near 0 = self-exciting/bursty arrivals)")

    # 5. depth-from-mid / k calibration
    #    mid immediately BEFORE row i is the orderbook state from row i-1
    #    (orderbook row i is the state AFTER message row i)
    mids_before = np.empty_like(mids)
    mids_before[0] = mids[0]
    mids_before[1:] = mids[:-1]
    deltas, edges, rates, counts, k, lnA, r2, frac_at_zero = depth_from_mid_and_intensity(
        times, etypes, prices, mids_before, session_seconds
    )
    print(f"fills at |delta| < 0.5 cent from prevailing mid: {frac_at_zero:.1%}")
    nz_bins = np.sum(counts > 0)
    print(f"depth histogram: {nz_bins}/{len(counts)} 1-cent bins populated "
          f"out of [0,{MAX_DEPTH_CENTS}) cents")
    if not np.isnan(k):
        print(f"lambda(delta) = A * exp(-k*delta) fit (delta in cents, "
              f"delta=0 bin excluded -- see code comment):")
        print(f"  k    = {k:.4f} /cent   A = exp({lnA:.3f}) = {np.exp(lnA):.4f} /sec   R^2 = {r2:.3f}")
    else:
        print("lambda(delta) fit: insufficient nonzero bins beyond delta=0 to fit")

    # 6. intraday seasonality (vol + fill rate, 30-min buckets)
    print("intraday seasonality (30-min buckets): time | sigma($/sqrt(s), 1s diffs) | fills")
    for t0, t1, vol, fcount in intraday_seasonality(times, mids, fill_times):
        h0, m0 = divmod(int(t0), 3600)
        h1, m1 = divmod(int(t1), 3600)
        print(f"  {h0:02d}:{(m0 % 3600) // 60:02d}-{h1:02d}:{(m1 % 3600) // 60:02d}  "
              f"sigma={vol:.4f}  fills={fcount:5d}")

    return {
        "ticker": ticker,
        "n_rows": len(times),
        "tick_size": ts,
        "sigma_1s": sigma_1s,
        "sigma_5s": sigma_5s,
        "autocorr_lag1": ac1,
        "autocorr_band": band,
        "iat_cv": cv,
        "frac_iat_under_10ms": frac_under_10ms,
        "frac_fills_at_zero_depth": frac_at_zero,
        "k_per_cent": k,
        "k_r2": r2,
        "n_fills": int(is_fill.sum()),
    }


def main():
    results = [analyze(t) for t in TICKERS]

    print(f"\n{'=' * 70}\nCROSS-SECTIONAL SUMMARY\n{'=' * 70}")
    header = f"{'ticker':6} {'tick$':>7} {'sigma_1s':>10} {'ac1':>8} {'CV(iat)':>9} {'%@0depth':>9} {'k/cent':>8} {'k R^2':>7}"
    print(header)
    for r in results:
        print(f"{r['ticker']:6} {r['tick_size']:7.4f} {r['sigma_1s']:10.5f} "
              f"{r['autocorr_lag1']:+8.4f} {r['iat_cv']:9.2f} "
              f"{r['frac_fills_at_zero_depth']:9.1%} {r['k_per_cent']:8.4f} {r['k_r2']:7.3f}")


if __name__ == "__main__":
    sys.exit(main())
