#!/usr/bin/env python3
"""
Phase 3: strategy-design signals. Phase 1 checked whether A-S's assumptions
hold; phase 2 refined the calibration (lit-only k, bid/ask split, session-
half stability). This phase asks a different question: what does the real
order flow itself say about how to DESIGN the strategy, independent of any
particular closed-form model?

Six analyses, per ticker:
  1. Markout / effective-realized-spread decomposition -- does passively
     providing liquidity (resting at the touch, getting filled) actually
     make money on this historical flow, and at what horizon does adverse
     selection eat the raw spread captured at execution? This is the
     single most direct answer to "is market making profitable here" this
     project can produce without running the strategy itself.
  2. Order lifetime / cancellation dynamics -- of orders placed, what
     fraction get cancelled vs. executed vs. survive to session close, and
     how fast? Directly informs how much queue risk a resting GTC quote
     actually faces.
  3. Order book imbalance (OBI) vs. forward returns -- a well-documented
     microstructure signal (Cont/Kukanov/Stoikov 2014): does L1 size
     imbalance predict short-horizon price direction here? If so, it's a
     candidate skew signal beyond pure inventory.
  4. Trade-sign autocorrelation -- is aggressor flow (buy/sell initiation)
     persistent in event-time? Positive autocorrelation means getting
     picked off once raises the odds of getting picked off again right
     after -- a direct amplifier of adverse-selection risk beyond what a
     single-fill markout captures.
  5. Order size distribution -- new-order, lit-fill, and hidden-fill size
     percentiles, needed to size the strategy's own quotes realistically
     rather than picking an arbitrary constant.
  6. Multi-lag autocorrelation profile (returns and squared returns) --
     phase 2 checked lag-1 only; this extends to lags 1/2/5/10/20/60 to
     see how quickly (or slowly) momentum/mean-reversion and volatility
     clustering decay.

Numpy-only, same policy as phase 1/2. Re-streams the raw CSVs (rather than
importing phase 1's stream_ticker) because this phase needs L1 sizes and
order_id, which phase 1/2 don't carry.
"""

import csv
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_dataset import DATASETS_DIR, SESSION_END_S, SESSION_START_S, TICKERS  # noqa: E402

MARKOUT_HORIZONS_S = [0.1, 0.5, 1.0, 5.0, 30.0]
ACF_LAGS_S = [1, 2, 5, 10, 20, 60]


def stream_full(ticker: str):
    """Adds order_id, ask_size_1, bid_size_1 to what phase 1's
    stream_ticker returns -- needed for lifetime tracking (order_id) and
    the OBI signal (L1 sizes)."""
    msg_path = DATASETS_DIR / f"{ticker}_2012-06-21_message_10.csv"
    ob_path = DATASETS_DIR / f"{ticker}_2012-06-21_orderbook_10.csv"

    times, etypes, oids, sizes, prices, directions = [], [], [], [], [], []
    ask_p, ask_sz, bid_p, bid_sz = [], [], [], []

    with open(msg_path, newline="") as mf, open(ob_path, newline="") as of:
        for mrow, orow in zip(csv.reader(mf), csv.reader(of)):
            times.append(float(mrow[0]))
            etypes.append(int(mrow[1]))
            oids.append(int(mrow[2]))
            sizes.append(int(mrow[3]))
            prices.append(int(mrow[4]) / 1e4)
            directions.append(int(mrow[5]))
            ask_p.append(int(orow[0]) / 1e4)
            ask_sz.append(int(orow[1]))
            bid_p.append(int(orow[2]) / 1e4)
            bid_sz.append(int(orow[3]))

    return dict(
        times=np.array(times),
        etypes=np.array(etypes),
        oids=np.array(oids),
        sizes=np.array(sizes),
        prices=np.array(prices),
        directions=np.array(directions),
        ask_p=np.array(ask_p),
        ask_sz=np.array(ask_sz),
        bid_p=np.array(bid_p),
        bid_sz=np.array(bid_sz),
    )


def pct(x, qs=(1, 5, 25, 50, 75, 95, 99)):
    return {q: float(np.percentile(x, q)) for q in qs} if len(x) else {q: float("nan") for q in qs}


def fmt_pct(p):
    return " ".join(f"p{q}={p[q]:.0f}" for q in p)


# ---------- 1. markout / effective-realized-spread decomposition ----------

def markout_decomposition(d, mids, mids_before):
    is_lit = d["etypes"] == 4
    t = d["times"][is_lit]
    trade_px = d["prices"][is_lit]
    mb = mids_before[is_lit]
    # aggressor sign: direction==+1 means a resting BUY was executed (an
    # incoming sell hit the bid); the PROVIDER (resting side) then holds a
    # +1 (long) position. direction==-1 -> provider holds -1 (short).
    provider_sign = d["directions"][is_lit].astype(float)

    spread_capture = provider_sign * (mb - trade_px) * 100.0  # cents/share
    print(f"  spread capture at execution (mean): {spread_capture.mean():+.3f}c/share  "
          f"(n={len(spread_capture):,}, this is the raw edge providing liquidity earns "
          f"before any subsequent price move)")

    print(f"  {'horizon':>8}  {'total P&L':>10}  {'= spread capture':>16}  {'+ markout':>10}   n_valid")
    for h in MARKOUT_HORIZONS_S:
        query_t = t + h
        valid = query_t <= SESSION_END_S
        idx = np.searchsorted(d["times"], query_t[valid], side="right") - 1
        idx = np.clip(idx, 0, len(mids) - 1)
        future_mid = mids[idx]
        total_pnl = provider_sign[valid] * (future_mid - trade_px[valid]) * 100.0
        markout = provider_sign[valid] * (future_mid - mb[valid]) * 100.0
        print(f"  {h:>6.1f}s  {total_pnl.mean():>+9.3f}c  {spread_capture[valid].mean():>+15.3f}c  "
              f"{markout.mean():>+9.3f}c   {valid.sum():,}")


# ---------- 2. order lifetime / cancellation dynamics ----------

def order_lifetime_dynamics(d):
    times, etypes, oids = d["times"], d["etypes"], d["oids"]
    submit_time = {}
    cancel_lifetimes = []
    fill_lifetimes = []
    n_submitted = 0

    for i in range(len(times)):
        et = etypes[i]
        oid = oids[i]
        if et == 1:
            submit_time[oid] = times[i]
            n_submitted += 1
        elif et == 3:  # full deletion
            t0 = submit_time.pop(oid, None)
            if t0 is not None:
                cancel_lifetimes.append(times[i] - t0)
        elif et == 4 or et == 5:  # execution (first fill = terminal for this metric)
            t0 = submit_time.pop(oid, None)
            if t0 is not None:
                fill_lifetimes.append(times[i] - t0)
        # et == 2 (partial cancel): non-terminal, order stays alive

    n_survived = len(submit_time)  # still open at session end (right-censored)
    n_cancel, n_fill = len(cancel_lifetimes), len(fill_lifetimes)
    denom = max(n_cancel + n_fill + n_survived, 1)
    print(f"  orders submitted: {n_submitted:,}  ->  "
          f"cancelled first: {n_cancel:,} ({n_cancel/denom:.1%})  "
          f"filled first: {n_fill:,} ({n_fill/denom:.1%})  "
          f"survived to close: {n_survived:,} ({n_survived/denom:.1%})")
    if n_cancel:
        cl = np.array(cancel_lifetimes)
        print(f"  time-to-cancel (s):     {fmt_pct(pct(cl))}")
    if n_fill:
        fl = np.array(fill_lifetimes)
        print(f"  time-to-first-fill (s): {fmt_pct(pct(fl))}")
    return n_submitted, n_cancel, n_fill, n_survived


# ---------- 3. order book imbalance vs forward returns ----------

def obi_predictive_power(d, mids, dt=1.0):
    times = d["times"]
    obi_raw = (d["bid_sz"] - d["ask_sz"]) / np.maximum(d["bid_sz"] + d["ask_sz"], 1)

    grid = np.arange(SESSION_START_S, SESSION_END_S, dt)
    idx = np.clip(np.searchsorted(times, grid, side="right") - 1, 0, len(times) - 1)
    obi_grid = obi_raw[idx]
    mid_grid = mids[idx]

    results = {}
    for horizon_steps, label in [(1, f"{dt:.0f}s"), (5, f"{5*dt:.0f}s")]:
        fwd_ret = np.empty_like(mid_grid)
        fwd_ret[:] = np.nan
        fwd_ret[:-horizon_steps] = mid_grid[horizon_steps:] - mid_grid[:-horizon_steps]
        valid = ~np.isnan(fwd_ret)
        x = obi_grid[valid] - obi_grid[valid].mean()
        y = fwd_ret[valid] - fwd_ret[valid].mean()
        denom = np.sqrt((x @ x) * (y @ y))
        corr = float((x @ y) / denom) if denom > 0 else float("nan")
        results[label] = corr
    n = len(grid)
    band = 1.96 / np.sqrt(n)
    print(f"  OBI vs forward return correlation: " +
          "  ".join(f"{lbl}: {c:+.4f}" for lbl, c in results.items()) +
          f"   (noise band +/-{band:.4f}, N~{n:,})")
    return results


# ---------- 4. trade-sign autocorrelation ----------

def generalized_autocorr(x: np.ndarray, lag: int) -> float:
    if lag >= len(x):
        return float("nan")
    x0, x1 = x[:-lag], x[lag:]
    x0c, x1c = x0 - x0.mean(), x1 - x1.mean()
    denom = np.sqrt((x0c @ x0c) * (x1c @ x1c))
    return float((x0c @ x1c) / denom) if denom > 0 else float("nan")


def trade_sign_autocorrelation(d):
    is_lit = d["etypes"] == 4
    aggressor_sign = -d["directions"][is_lit].astype(float)  # provider_sign negated
    n = len(aggressor_sign)
    band = 1.96 / np.sqrt(n) if n > 0 else float("nan")
    lags = [1, 5, 20]
    acs = [generalized_autocorr(aggressor_sign, lag) for lag in lags]
    print(f"  trade-sign autocorr (event-time, n={n:,}, band +/-{band:.4f}): " +
          "  ".join(f"lag{lag}: {ac:+.4f}" for lag, ac in zip(lags, acs)))
    return acs


# ---------- 5. size distributions ----------

def size_distributions(d):
    new_sizes = d["sizes"][d["etypes"] == 1]
    lit_sizes = d["sizes"][d["etypes"] == 4]
    hidden_sizes = d["sizes"][d["etypes"] == 5]
    print(f"  new-order size (shares):  {fmt_pct(pct(new_sizes))}")
    print(f"  lit-fill size (shares):   {fmt_pct(pct(lit_sizes))}")
    print(f"  hidden-fill size (shares):{fmt_pct(pct(hidden_sizes))}")


# ---------- 6. multi-lag ACF ----------

def multi_lag_acf(diffs_1s):
    sq = diffs_1s ** 2
    n = len(diffs_1s)
    band = 1.96 / np.sqrt(n)
    ret_acf = [generalized_autocorr(diffs_1s, lag) for lag in ACF_LAGS_S]
    arch_acf = [generalized_autocorr(sq, lag) for lag in ACF_LAGS_S]
    print(f"  return ACF   (lags {ACF_LAGS_S}s, band +/-{band:.4f}): " +
          "  ".join(f"{a:+.4f}" for a in ret_acf))
    print(f"  |ret|^2 ACF  (lags {ACF_LAGS_S}s, band +/-{band:.4f}): " +
          "  ".join(f"{a:+.4f}" for a in arch_acf))
    return ret_acf, arch_acf


def analyze_signals(ticker: str):
    print(f"\n{'=' * 70}\n{ticker} -- strategy-design signals\n{'=' * 70}")
    d = stream_full(ticker)
    mids = (d["ask_p"] + d["bid_p"]) / 2.0
    mids_before = np.empty_like(mids)
    mids_before[0] = mids[0]
    mids_before[1:] = mids[:-1]

    print("[1] markout / effective-realized-spread decomposition (provider's side):")
    markout_decomposition(d, mids, mids_before)

    print("[2] order lifetime / cancellation dynamics:")
    order_lifetime_dynamics(d)

    print("[3] order book imbalance vs forward returns:")
    obi_predictive_power(d, mids)

    print("[4] trade-sign autocorrelation:")
    trade_sign_autocorrelation(d)

    print("[5] size distributions:")
    size_distributions(d)

    print("[6] multi-lag autocorrelation profile:")
    grid = np.arange(SESSION_START_S, SESSION_END_S, 1.0)
    idx = np.clip(np.searchsorted(d["times"], grid, side="right") - 1, 0, len(mids) - 1)
    diffs_1s = np.diff(mids[idx])
    multi_lag_acf(diffs_1s)


def main():
    for t in TICKERS:
        analyze_signals(t)


if __name__ == "__main__":
    sys.exit(main())
