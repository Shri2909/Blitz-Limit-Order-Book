#!/usr/bin/env python3
"""
Re-derives gamma_default, skew_strength_default, and sigma_cents_buckets for
every ticker, fixing two issues found while auditing the first backtest run
(new-feature/RESULTS.md, "What this actually shows -- and what it doesn't"):

  BUG 1 -- sigma_cents_buckets normalization mismatch. export_calibration.py's
  sigma_buckets() computed np.std(np.diff(sub)) on RAW tick-level mids inside
  each half-hour window -- no fixed-dt resampling, no /sqrt(dt) scaling. The
  session-level sigma used everywhere else (realized_vol_arithmetic, and the
  session-level sigma gamma_default itself was calibrated against) resamples
  onto a fixed 1-second grid first. Real order-book updates arrive far more
  than once per second, so raw tick-to-tick diffs are systematically smaller
  than properly time-scaled 1-second innovations -- verified directly: AAPL's
  bucket-0 sigma was 2.15c under the old (buggy) method vs. 5.69c under the
  same 1s-grid method realized_vol_arithmetic uses, a ~2.6x understatement,
  worse in some buckets. Since sigma_for_time() feeds sigma^2 into BOTH the
  reservation-price skew and the k-based half-spread at runtime, this alone
  silently shrank live inventory control (and, for STANDARD_K names, the
  spread itself) well below what gamma_default's own calibration assumed.
  Fixed here by reusing realized_vol_arithmetic's own 1s-grid methodology
  per bucket, not a separate ad hoc metric.

  BUG 2 (design gap, not a code bug) -- gamma_default was solved at an
  extreme corner: q=q_max (a 3x safety-padded cap, RUN_LENGTH_SAFETY_MULTIPLIER
  in analyze_dataset_riskparams.py) AND T-t=T_max (full session remaining,
  i.e. market open). skew_strength_default, by contrast, was solved at a
  genuinely typical condition (1 signal std). Two incomparable operating
  points: in real mid-session operation, inventory rarely reaches q_max and
  T-t has shrunk from its start-of-session maximum, so the inventory term
  ran far below its design target while the signal term ran at full
  strength essentially always. Fixed by re-targeting gamma_default's design
  point at q_typical = q_max / RUN_LENGTH_SAFETY_MULTIPLIER (the expected
  size of ONE ordinary directional run -- already implicit in how q_max
  itself is built, not a new free parameter) and T_typical = T_max / 2 (the
  average remaining time under a uniform arrival assumption, not the
  best-case maximum). q_max itself is untouched -- it stays the padded hard
  cap that triggers one-sided quote suppression; only the CALIBRATION
  target point moves.

Both fixes only touch calibration.hpp's generated constants -- no change to
strategy.hpp's formulas, no change to q_max's own value or role.
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_dataset import (  # noqa: E402
    SESSION_END_S,
    SESSION_START_S,
    TICKERS,
    realized_vol_arithmetic,
    tick_size,
)
from analyze_dataset_alpha import compute_ofi_events  # noqa: E402
from analyze_dataset_alpha2 import MOMENTUM_WINDOW_S, grid_index, stream_depth  # noqa: E402
from analyze_dataset_alpha3 import build_component_grids, zscore  # noqa: E402
from analyze_dataset_deep import fit_lambda_delta, time_weighted_mean_and_frac_below  # noqa: E402
from analyze_dataset_riskparams import RUN_LENGTH_SAFETY_MULTIPLIER  # noqa: E402
from analyze_dataset_signals import generalized_autocorr  # noqa: E402

TICKERS_WITH_MICRO = {"INTC", "MSFT"}
N_BUCKETS = 13
TARGET_FRACTION = 0.5  # free, documented choice -- unchanged from final_plan.md
T_MAX = SESSION_END_S - SESSION_START_S


def sigma_buckets_fixed(times, mids):
    """Same 1-second-grid-resample + /sqrt(dt) methodology as
    realized_vol_arithmetic, applied within each half-hour window --
    replaces export_calibration.py's raw-tick-diff version (BUG 1).

    mids is dollar-denominated (same convention as everywhere else in the
    Python analysis) -- returns CENTS, matching calibration.hpp's
    sigma_cents_buckets convention, by applying the exact same *100
    conversion the UNITS WARNING in calibration.hpp already documents for
    the session-level figure. Forgetting this *100 here would reintroduce
    the identical class of units bug on the buckets array instead of the
    scalar -- this comment exists because that happened once already while
    writing this script."""
    edges = np.linspace(SESSION_START_S, SESSION_END_S, N_BUCKETS + 1)
    out = []
    for i in range(N_BUCKETS):
        t0, t1 = edges[i], edges[i + 1]
        grid = np.arange(t0, t1, 1.0)
        idx = np.searchsorted(times, grid, side="right") - 1
        idx = np.clip(idx, 0, len(mids) - 1)
        grid_mids = mids[idx]
        diffs = np.diff(grid_mids)
        sigma_dollars = float(np.std(diffs)) if len(diffs) > 1 else float("nan")
        out.append(sigma_dollars * 100.0)
    return out


def k_bid_ask(d, mids_before, session_seconds):
    is_lit = d["etypes"] == 4
    deltas_lit = np.abs(d["prices"][is_lit] - mids_before[is_lit]) * 100.0
    lit_dir = d["directions"][is_lit]
    k_bid, _, _, _ = fit_lambda_delta(deltas_lit[lit_dir == 1], session_seconds)
    k_ask, _, _, _ = fit_lambda_delta(deltas_lit[lit_dir == -1], session_seconds)
    return k_bid, k_ask


def mean_spread_cents(times, ask_p1, bid_p1, prices, etypes):
    spread_cents = (ask_p1 - bid_p1) * 100.0
    ts_lit = tick_size(prices[etypes == 1])
    min_tick_cents = ts_lit * 100.0
    mean_spread, _ = time_weighted_mean_and_frac_below(
        times, spread_cents, SESSION_END_S, min_tick_cents + 1e-6
    )
    return mean_spread


def q_max_and_typical(d):
    is_lit = d["etypes"] == 4
    aggressor_sign = -d["directions"][is_lit].astype(float)
    median_lit_fill_size = float(np.median(d["sizes"][is_lit]))
    lags = [1, 5, 20]
    acs = [generalized_autocorr(aggressor_sign, lag) for lag in lags]
    x = np.array(lags, dtype=float)
    y = np.log(np.array(acs))
    slope, _ = np.polyfit(x, y, 1)
    rho = float(np.exp(slope))
    expected_run_length = 1.0 / (1.0 - rho) if rho < 1.0 else float("inf")
    q_typical = expected_run_length * median_lit_fill_size
    q_max = RUN_LENGTH_SAFETY_MULTIPLIER * q_typical
    return q_max, q_typical, median_lit_fill_size


def main():
    print("// AUTO-GENERATED by new-feature/analysis/derive_gamma_skew.py -- do not hand-edit")
    print("// Re-derivation fixing the sigma-bucket normalization bug and the")
    print("// corner-vs-typical gamma_default mismatch -- see this script's docstring.")
    print()

    for ticker in TICKERS:
        d = stream_depth(ticker)
        mids = (d["ask_p1"] + d["bid_p1"]) / 2.0
        mids_before = np.empty_like(mids)
        mids_before[0] = mids[0]
        mids_before[1:] = mids[:-1]
        session_seconds = T_MAX

        sigma_session_dollars, _ = realized_vol_arithmetic(d["times"], mids, dt=1.0)
        sigma_session_cents = sigma_session_dollars * 100.0

        sig_buckets_old = None  # not recomputed here; see export_calibration.py for the buggy version
        sig_buckets_new = sigma_buckets_fixed(d["times"], mids)

        k_bid, k_ask = k_bid_ask(d, mids_before, session_seconds)
        mean_spread = mean_spread_cents(d["times"], d["ask_p1"], d["bid_p1"], d["prices"], d["etypes"])
        q_max, q_typical, quote_size = q_max_and_typical(d)

        grid = np.arange(SESSION_START_S, SESSION_END_S, 1.0)
        idx_last = grid_index(d["times"], grid)
        bucket_idx = np.clip(np.searchsorted(grid, d["times"], side="right") - 1, 0, len(grid) - 1)
        ofi_grid, mom_grid, micro_grid = build_component_grids(d, grid, idx_last, bucket_idx)
        ofi_std = float(ofi_grid.std())

        # ---- fixed derivation: q_typical / T_typical, not q_max / T_max ----
        t_typical = T_MAX / 2.0
        gamma_default = TARGET_FRACTION * mean_spread / (q_typical * sigma_session_cents ** 2 * t_typical)

        if ticker in TICKERS_WITH_MICRO:
            components = {"OFI": ofi_grid, "TradeFlowMom": mom_grid, "MicroDev": micro_grid}
            z = [zscore(v) for v in components.values()]
            mid_grid = mids[idx_last]
            fwd = np.full_like(mid_grid, np.nan)
            fwd[:-5] = mid_grid[5:] - mid_grid[:-5]
            valid = ~np.isnan(fwd)
            X = np.column_stack(z)[valid]
            y = fwd[valid]
            beta, *_ = np.linalg.lstsq(np.column_stack([X, np.ones(len(X))]), y, rcond=None)
            composite_score = X @ beta[:-1]
            signal_std = float(composite_score.std())
            weights = (float(beta[0]), float(beta[1]), float(beta[2]), float(beta[3]))
        else:
            signal_std = ofi_std
            weights = None

        skew_strength_default = TARGET_FRACTION * mean_spread / signal_std

        print(f"// ---- {ticker} ----")
        print(f"// mean_spread_cents={mean_spread:.6f}  quote_size(median lit fill)={quote_size:.0f}"
              f"  q_max={q_max:.0f}  q_typical(=q_max/{RUN_LENGTH_SAFETY_MULTIPLIER:.0f})={q_typical:.1f}")
        print(f"// sigma session-level (cents/sqrt s) = {sigma_session_cents:.6f}")
        print("// sigma_cents_buckets (FIXED, 1s-grid normalized):")
        print("//   " + ", ".join(f"{v:.6f}" for v in sig_buckets_new))
        if weights is not None:
            print(f"// composite OLS weights (OFI, TradeFlowMom, MicroDev, intercept): "
                  f"{weights[0]:.8f}, {weights[1]:.8f}, {weights[2]:.8f}, {weights[3]:.8f}")
        else:
            print(f"// k_bid={k_bid:.6f}/c  k_ask={k_ask:.6f}/c")
        print(f"// signal_std used for skew_strength_default = {signal_std:.6f}")
        print(f"gamma_default[{ticker}] = {gamma_default:.6e}")
        print(f"skew_strength_default[{ticker}] = {skew_strength_default:.6e}")
        print()


if __name__ == "__main__":
    sys.exit(main())
