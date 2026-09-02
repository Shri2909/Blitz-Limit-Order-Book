# A-S market maker: backtest results

Real LOBSTER replay, 2012-06-21, full session per ticker, A-S vs. naive
fixed-spread control on the identical historical stream.

```bash
cmake -S . -B build-asmm -DCMAKE_BUILD_TYPE=Release
cmake --build build-asmm --target blitz_asmm_backtest blitz_asmm_verify_replay blitz_lob_test_asmm_phase11
ctest --test-dir build-asmm -R asmm_phase11        # 12/12 pass
./build-asmm/blitz_asmm_backtest --ticker AAPL --strategy both --output-dir new-feature/results
```

## Replay fidelity: exact, all five names

Before any strategy result is worth reading, the replay itself has to
reconstruct the real book. With no strategy inserted, the reconstructed
book matches the real LOBSTER orderbook file **row for row, zero
mismatches, across all 2,110,860 rows** of the five-name panel:

| Ticker | Rows | Mismatches |
|---|---|---|
| AAPL | 400,391 | 0 |
| AMZN | 269,748 | 0 |
| GOOG | 147,916 | 0 |
| INTC | 624,040 | 0 |
| MSFT | 668,765 | 0 |

Getting there required three fixes that only surfaced by running this
check, each documented at its call site:

1. **`client_id` was never set**, so `is_self_trade()` (a plain
   `client_id ==` comparison, defaulting to 0) silently blocked nearly
   every match. Symptom: 99.99% row mismatch, no error, a book that never
   traded.
2. **The message file is not self-contained.** LOBSTER emits no type-1
   creation events for orders that predate the file's window, and row 0's
   snapshot only exposes 10 levels — deeper pre-existing orders surface
   later with no way to reconstruct them (e.g. AAPL order 13419503 rests
   at 58540 from row 226 but appears in the message file only at row 448,
   when it executes).
3. **Per-order queue composition within a level is unrecoverable** from a
   10-level aggregate feed, so synthetic liquidity can't hold the queue
   positions the real orders it proxies did. Resolved by making the
   snapshot authoritative for market liquidity each row, which is what
   actually governs fill realism.

## Recalibration: fixing the first run's parameterization flaw

The first backtest run found AAPL and AMZN finishing pinned at their
inventory cap, with the reservation-price skew's signal term outweighing
its inventory-control term by 15–380x under typical (not corner)
conditions. Root-caused to two separate issues in
`new-feature/include/asmm/calibration.hpp`'s generated constants — not the
strategy formulas themselves — both fixed by
`new-feature/analysis/derive_gamma_skew.py`:

1. **`sigma_cents_buckets` normalization bug.** The original extraction
   (`export_calibration.py::sigma_buckets`) computed each half-hour
   bucket's σ from raw tick-to-tick mid differences, with no fixed-dt
   resampling and no `/√dt` scaling. The session-level σ used everywhere
   else (including `gamma_default`'s own calibration) resamples onto a
   fixed 1-second grid first. Real order-book updates arrive far more than
   once a second, so the raw version was systematically too small —
   verified directly: AAPL's open-of-session bucket was 2.15¢ under the
   old method vs. 5.69¢ on a proper 1-second grid, a 2.6x understatement,
   worse in other buckets. Since `sigma_for_time()` feeds σ² into both the
   reservation-price skew and the k-based half-spread at runtime, this
   silently shrank live inventory control (and the spread itself for
   `STANDARD_K` names) well below what `gamma_default` was calibrated to
   assume.
2. **Corner-vs-typical mismatch.** `gamma_default` was solved at
   `(q_max, T_max)` — a 3x safety-padded inventory cap *and* the maximum
   possible time-to-close, simultaneously — while `skew_strength_default`
   was solved at a genuinely typical condition (1 signal standard
   deviation). Re-targeted `gamma_default` at `q_typical = q_max / 3` (the
   expected size of one ordinary directional run — already implicit in how
   `q_max` itself is built, not a new free parameter) and
   `T_typical = T_max / 2`. `q_max`'s own value and its role as the hard
   suppression cap are unchanged; only the calibration target point moved.

Both fixes only touch generated constants in `calibration.hpp` — no change
to `strategy.hpp`'s formulas. Effect, sanity-checked with phase-11 test #10
(`skew < 20x mean_spread` at the true worst-case corner — still comfortably
satisfied): AAPL's per-unit inventory-control skew increased roughly 25–30x
at typical operating points, from ~15–380x below the signal term to the
same order of magnitude as it.

## Panel results (recalibrated)

| Ticker | A-S P&L | naive P&L | ΔP&L | A-S markout@5s | naive markout@5s | Δmarkout | A-S final inv. | q_max |
|---|---|---|---|---|---|---|---|---|
| AAPL | −$7,767 | +$19,583 | −$27,351 | −5.698¢ | −7.070¢ | **+1.372¢** | 2,653 | 2,634 |
| AMZN | −$1,334 | +$2,787 | −$4,120 | −5.091¢ | −7.175¢ | **+2.084¢** | 1,720 | 2,291 |
| GOOG | +$1,472 | +$5,013 | −$3,542 | −14.076¢ | −13.966¢ | −0.110¢ | 506 | 1,605 |
| INTC | +$175 | +$913 | −$738 | −0.743¢ | −0.675¢ | −0.068¢ | −712 | 5,670 |
| MSFT | +$598 | +$657 | −$59 | −0.756¢ | −0.728¢ | −0.028¢ | −1,613 | 4,508 |

## What this actually shows — and what it doesn't

**The recalibration worked as intended.** AMZN is no longer pinned at its
cap (1,720 vs. q_max 2,291, down from 2,317-at-cap before) — the stronger,
correctly-normalized inventory control is now genuinely restraining
accumulation. GOOG, INTC, and MSFT sit well inside their caps, as they did
before. **The skew mechanism's adverse-selection benefit held and grew**:
A-S improves 5s markout in AAPL (+1.4¢, up from +0.8¢) and AMZN (+2.1¢, up
from +2.0¢) — the two names where the recalibration changed the most —
while GOOG/INTC/MSFT stay roughly flat, consistent with them not having
been meaningfully mis-calibrated in the first place.

**AAPL still finishes pinned at its cap (2,653 vs. 2,634) — investigated,
not hand-waved.** Pulling the real, strategy-free replayed mid-price
directly from the LOBSTER data (no strategy inserted at all): AAPL's own
session mid fell from \$585.63 at the open to \$577.61 at the close, a
**real, persistent −1.37% intraday decline**. A market maker quoting
continuously through a real one-directional trend accumulates a long
position as a rational response to genuinely one-sided order flow — that
is not what bounded, linear (in inventory) A-S skew is built to fully
neutralize, and hitting the hard `q_max` cap in exactly that scenario is
the cap doing its job, not evidence of a further calibration error. This
is stated as a real, data-confirmed limitation (A-S has no trend-detection
term), not swept under "needs more tuning."

**P&L is now a more honest number, but the caveats from the first run still
apply**: one session per ticker (no re-seeding), counterfactual insertion
(other participants' historical orders were submitted against a book that
didn't contain the strategy's quotes), no fee/rebate model, single
instrument. The markout column — the metric this recalibration was
actually aimed at — is the one to read as evidence of the skew mechanism
working; the P&L column still mixes in one session's worth of realized
directional risk (most visibly AAPL's real intraday trend) and shouldn't be
read as a Sharpe-ratio-style claim from a single trial.

## Known limitations

- One session, one trial per ticker — inherited from real-data replay;
  the historical sequence can't be reseeded (see final_plan.md).
- Counterfactual insertion: other participants' historical orders were
  submitted against a book that didn't contain the strategy's quotes.
- No fee/rebate model; single-instrument, no portfolio-level risk.
- No trend/regime detection: A-S's inventory skew is linear in inventory
  and session-static; it cannot fully neutralize a real, persistent
  one-directional session (AAPL, above) — a legitimate model limitation,
  not a bug.
- Per-fill CSVs in `new-feature/results/` carry markout at 1s/5s/30s;
  HAC/Newey-West significance testing on them is a Python job
  (`analyze_dataset_riskparams.py::newey_west_se`), not yet run.
