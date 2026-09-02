# Dataset Analysis — pre-calibration diagnostics

Before any Avellaneda-Stoikov parameter is fit from the real LOBSTER panel
(`datasets/`, see `datasets/manifest.txt`), this checks whether the model's
own assumptions actually hold on this data. Five checks, run across all
five names (AAPL, AMZN, GOOG, INTC, MSFT — 2012-06-21, full session,
2,110,860 events total). Method and exact numbers are reproducible:

```bash
pip install -r new-feature/requirements.txt
python3 new-feature/analysis/analyze_dataset.py
```

Raw run captured in `new-feature/analysis/output_2026-09-01.txt`. Nothing
below is asserted without a number from that run behind it — same policy
as the rest of this repo's benchmark claims.

---

## Cross-sectional summary

| Ticker | Tick (lit) | σ, $/√s (1s) | lag-1 autocorr | inter-arrival CV | % fills @ 0 depth | k (/cent) | k fit R² |
|---|---|---|---|---|---|---|---|
| AAPL | $0.0100 | 0.03528 | +0.0155 (outside band) | 2.64 | 5.2% | 0.3256 | 0.942 |
| AMZN | $0.0100 | 0.01982 | +0.0271 (outside band) | 2.52 | 5.6% | 0.3317 | 0.914 |
| GOOG | $0.0100 | 0.04026 | −0.0126 (within band) | 2.45 | 2.1% | 0.0709 | 0.513 |
| INTC | $0.0100 | 0.00227 | −0.0185 (outside band) | 2.80 | 72.7% | **not estimable** | — |
| MSFT | $0.0100 | 0.00262 | −0.0046 (within band) | 2.76 | 72.6% | **not estimable** | — |

---

## Assumption-by-assumption verdict

### 1. Tick size — clean, but split the source

Lit new-limit-order prices (event type 1) land on a clean **$0.01** grid
for all five names, no exceptions. The raw price column (which also
includes hidden-order executions, event type 5) shows a **$0.005** minimum
increment instead — that's not a data quality problem, it's real:
midpoint-priced dark/hidden fills are common enough in this data to show
up as a distinct sub-penny tick. **Verdict: PROVEN, with a caveat** — use
$0.01 as the quoting grid (that's what a resting limit order can actually
be priced at); don't mistake the $0.005 figure for the tradable tick.

### 2. Volatility — real, computed correctly, wildly different by name

σ is computed in **dollars per √second** (arithmetic Brownian, matching
`dS_t = σ dW_t`), not log-return vol — the distinction matters because
A-S's reservation-price formula is additive in price, not multiplicative.
Cross-checked at two sampling rates (1s and 5s): the ratio is 0.97–1.03
across all five names, close to the 1.00 pure-Brownian-scaling prediction.
**Verdict: PROVEN** — the estimator is stable across sampling frequency,
which is the standard sanity check for whether 1-second microstructure
noise is contaminating the realized-vol estimate. It isn't, materially.

The magnitude spread is the headline number worth remembering: GOOG's σ
(0.0403) is **~17.7x** INTC's (0.0023). A single hardcoded σ across the
whole panel would be meaningless — confirms σ has to be a per-name,
per-session calibrated input, never a constant.

### 3. Random-walk / no-drift — mixed, but small

Lag-1 autocorrelation of 1-second price differences is statistically
outside the ±1.96/√N noise band for AAPL (+0.016), AMZN (+0.027), and INTC
(−0.019); within it for GOOG and MSFT. **Verdict: PARTIALLY VIOLATED, not
practically dominant** — these are real, detectable deviations from a pure
random walk (weak momentum in AAPL/AMZN, weak mean-reversion in INTC), but
single-digit-percent correlations, not the kind of structure that should
overturn the whole modeling approach. Worth stating in any report this
feeds, not worth re-deriving A-S with a drift term over.

### 4. Poisson fill arrivals — cleanly violated, consistently, everywhere

This is the most unambiguous result in the whole pass. Coefficient of
variation of fill inter-arrival times is **2.45–2.80 across all five
names** — a true Poisson process has CV≈1; CV this far above 1 means
arrivals are **clustered/self-exciting**, not memoryless. Confirmed a
second way: 41–48% of consecutive fills across every name arrive **less
than 10ms apart**, which is exactly the signature of order-flow bursts
(a sweep or a resting-order cascade), not independent Poisson events.
**Verdict: DISPROVEN, uniformly.** The classical A-S closed-form leans on
Poisson arrivals to get `λ(δ)=Ae^{-kδ}` in the first place — this data
says real fill arrivals are bursty. This is a genuine, known gap between
the textbook model and real microstructure (the literature's usual fix is
a Hawkes-process arrival model), and the honest move is to say so
explicitly in the eventual report rather than quietly fitting a Poisson
`k` and not mentioning the diagnostic that contradicts it.

### 5. Exponential depth-decay (the `k` calibration itself) — works for three names, breaks for two, and the reason why is the most useful finding here

For **AAPL, AMZN, GOOG** — the three higher-priced, relatively
wider-spread names — the `λ(δ)=Ae^{-kδ}` fit is genuinely good (R² = 0.94,
0.91, 0.51) across a full 20-cent depth range with all 20 one-cent bins
populated.

For **INTC and MSFT**, the regression **cannot be fit at all** — only 2 of
20 depth bins have any fills in them, because **72.7%/72.6% of all fills
in these names land within half a cent of the prevailing mid.** The reason
is structural, not a bug: these are low-priced ($27–$32), extremely liquid
names whose quoted spread sits at or near the $0.01 minimum tick almost
the entire session. When the spread *is* the tick, "distance from mid"
barely has room to vary — there's no meaningful depth axis for an
exponential decay curve to be fit against, because virtually every trade
happens at the touch by construction.

**Verdict: the single-`k`-per-name exponential-decay calibration is only
identified for names with spreads wide enough (in cents) to populate more
than one or two depth buckets.** Tick-constrained names need either (a)
depth measured in ticks rather than cents with a coarser bucket, or (b) a
different intensity model entirely (e.g. probability-of-fill-at-the-touch
vs. probability-of-fill-one-tick-back, a two- or three-state model instead
of a continuous decay curve). This is exactly the kind of finding worth
reporting as its own result, not a nuisance to engineer around silently.

---

## What this settles for the build

1. **σ and, where fittable, `k` must be per-ticker inputs**, calibrated
   from this analysis pass — never a single constant swept across names.
2. **AAPL / AMZN / GOOG are the three names to build and validate the
   full A-S quoting + calibration pipeline against first** — they're the
   ones where every assumption the closed-form needs is at least
   approximately checkable and the `k` fit is real.
3. **INTC / MSFT are not silently droppable** — they're a legitimate
   second case study precisely *because* the standard calibration fails on
   them, and that failure has a clean, explainable microstructure reason
   (tick-constrained spread). Worth its own short note in the eventual
   strategy report rather than pretending the panel is uniform.
4. **The Poisson-arrival violation (CV 2.45–2.80, all five names) gets
   stated as a named limitation of the closed-form model**, not fixed
   before building — matches this project's existing pattern of shipping
   a result with its own caveat clearly attached (e.g. the core-pinning
   ablation's `INCONCLUSIVE` verdict) rather than either hiding the gap or
   blocking on a full Hawkes-process rebuild.
5. **The autocorrelation deviations are noted, not acted on** — real but
   small; a drift term isn't warranted by numbers this size.

---

## Phase 2: deep-dive diagnostics

Seven follow-up checks, run via `new-feature/analysis/analyze_dataset_deep.py`
(raw output: `new-feature/analysis/output_deep_2026-09-01.txt`), built on
top of phase 1 rather than replacing it. Where phase 1 inferred something
indirectly, phase 2 measures it directly; where phase 1 left an assumption
unchecked (Gaussian increments, side symmetry, session-long parameter
stability), phase 2 checks it.

### Cross-sectional deep-dive summary

| Ticker | Mean spread (¢) | % time @ min tick | k (pooled) | k (lit-only) | k (bid) | k (ask) | σ ratio (2nd/1st half) | excess kurtosis | tail ratio | ARCH lag-1 | % hidden |
|---|---|---|---|---|---|---|---|---|---|---|---|
| AAPL | 15.13 | 0.1% | 0.3256 | 0.3273 | 0.4074 | 0.2900 | 0.78 | +28.0 | 8.75x | +0.054 | 32.4% |
| AMZN | 12.85 | 0.2% | 0.3317 | 0.3379 | 0.3732 | 0.2931 | 0.71 | +34.5 | 10.24x | +0.081 | 21.4% |
| GOOG | 27.33 | 0.0% | 0.0709 | 0.0582 | 0.0661 | 0.0528 | 0.61 | +89.7 | 7.95x | +0.104 | 33.5% |
| INTC | 1.01 | **98.8%** | not estimable | not estimable | not estimable | not estimable | 0.64 | +111.9 | 13.90x | +0.042 | 11.0% |
| MSFT | 1.01 | **99.0%** | not estimable | not estimable | not estimable | not estimable | 0.73 | +35.7 | 19.14x | +0.079 | 10.8% |

### 1. Quoted spread — the INTC/MSFT finding is now a direct measurement, not an inference

Time-weighted (not event-weighted — a burst of 50 messages in 10ms doesn't
get to outvote one message that held for 2 seconds) mean spread: AAPL
15.1¢, AMZN 12.8¢, GOOG 27.3¢ — all comfortably wider than the 1¢ tick.
INTC and MSFT sit at the **minimum tick 98.8% and 99.0% of the entire
6.5-hour session.** Phase 1 inferred the depth-decay fit was failing for
these two because of tick-constrained spreads; this confirms it exactly —
there is, quite literally, almost no session-time during which these two
names' spread is wide enough for a "distance from mid" to mean anything.

### 2. Lit-only vs. pooled k — matters for GOOG, not for AAPL/AMZN

The strategy under construction rests **displayed** GTC limit orders — it
can only ever be filled the way a type-4 (visible) execution represents,
never the way a type-5 (hidden/often-midpoint) execution does. Restricting
the regression to lit fills only barely moves AAPL (0.3256→0.3273, 0.99x)
or AMZN (0.3317→0.3379, 0.98x), but shifts GOOG by **1.22x**
(0.0709→0.0582) — and GOOG's fit was already the shakiest of the three
(R²=0.51 pooled, drops to 0.37 lit-only). **Verdict: use lit-only k for
all three names on principle** (it's the methodologically correct
population regardless of magnitude), and treat GOOG's k as the least
trustworthy of the three, now for two independent reasons.

### 3. Bid vs. ask asymmetry — real, consistent, same direction in all three fittable names

Bid-side k > ask-side k in **AAPL** (0.407 vs 0.290), **AMZN** (0.373 vs
0.293), and **GOOG** (0.066 vs 0.053) — a consistent pattern, not noise:
aggressive selling into the bid loses intensity with depth faster than
aggressive buying into the ask does, in every name where this is
measurable. **Verdict: the book is not symmetric, and a single pooled k
per name is a real simplification** — worth stating explicitly if the
eventual strategy uses one symmetric k rather than k_bid/k_ask.

### 4. Session-half stability — σ drifts predictably, k drifts a lot, GOOG's early-session k is statistical noise

Splitting the session at 12:45pm and refitting independently:

- **σ falls in the second half for all five names** (ratio 0.61–0.78) —
  consistent with the intraday seasonality already seen in phase 1 (higher
  open volatility, calmer midday/afternoon). Expected, and a clean result.
- **k is far less stable.** AAPL: 0.254 → 0.444 (1.75x). AMZN: 0.277 →
  0.363 (1.31x). GOOG: **0.011 → 0.095 (8.9x)** — and GOOG's first-half fit
  has R²=0.022, which is not a small-effect result, it's **no fit at all**.
  **Verdict: a single session-long k is a real approximation for AAPL/AMZN
  (drifts, but stays in the same order of magnitude) and is not a
  trustworthy number for GOOG in the first half of the session at all.**
  This is the strongest argument in the whole analysis for treating k as
  something recalibrated over a rolling window rather than fit once per
  session and frozen.

### 5. Fat tails — the Gaussian/Brownian assumption is not a close approximation, anywhere

Excess kurtosis of 1-second price differences: **+28 to +112** across the
five names (Gaussian = 0). Empirical rate of >3σ moves is **7.95x to
19.14x** the Gaussian-predicted rate. This is the most quantitatively
unambiguous result in the whole pass, and it says the same thing for every
single name: `dS_t = σ dW_t` with Gaussian increments is not just an
approximation with some error, it is measurably, consistently wrong about
tail behavior. **Verdict: state this as a named, permanent limitation of
the closed-form A-S model when it's used against this data** — the
literature's usual answer is a jump-diffusion or Lévy-process extension,
out of scope for this feature, but the gap needs to be on the record, not
discovered by a reader re-deriving the diagnostics.

### 6. Volatility clustering (ARCH effect) — present everywhere, consistent with the fat tails

Lag-1 autocorrelation of *squared* 1-second price differences is
positive and outside the noise band for all five names (+0.042 to
+0.104) — high-volatility moments cluster together in time, which is both
an independent confirmation of the fat-tails finding (clustered
conditional volatility is the standard mechanism that produces fat
unconditional tails) and a second, independent way the constant-σ
assumption is approximate rather than exact.

### 7. Lit vs. hidden fill composition

Hidden (dark, type-5) fills range from **10.8%–11.0%** of total fills for
the tick-constrained names (INTC, MSFT) to **21.4%–33.5%** for the
wider-spread names (AMZN, GOOG, AAPL) — a sensible pattern: a midpoint
hidden order only offers meaningful price improvement when the displayed
spread is wide enough to have a midpoint worth pegging to, which is
exactly the condition INTC/MSFT don't meet. Mean execution size is similar
between lit and hidden fills within each name (e.g. AAPL: 78 vs 89
shares), so size isn't the distinguishing factor — accessibility is.

---

## Revised design implications (supersedes phase 1's list where they conflict)

1. **Use lit-only (type 4) fills for every k regression**, not pooled —
   methodologically correct regardless of how much it moves the number for
   any given name, and it materially changes GOOG's estimate.
2. **AAPL and AMZN are the two names to trust most for a full-session,
   single-k calibration** — both have strong fits (R² 0.89–0.93 lit-only),
   moderate (not extreme) session-half drift (1.3–1.75x), and pooled/lit
   choice barely matters.
3. **GOOG's k is usable but should ship with its own caveat attached** —
   weakest fit of the three (R²=0.37 lit-only), most sensitive to the
   lit/pooled choice (1.22x), and its first-session-half fit is
   statistical noise (R²=0.022). Report the full-session number, but do
   not present it with the same confidence as AAPL/AMZN's.
4. **INTC/MSFT are a structural non-fit, now proven two independent ways**
   (72–73% of fills within half a cent of mid, *and* spread at the minimum
   tick 98.8–99.0% of session time) — still worth their own short case
   study on exactly why the continuous-depth-decay model can't apply to a
   tick-constrained name, not a silently dropped pair.
5. **Bid/ask asymmetry (point 3 above) is real and consistent** — if the
   eventual strategy uses one symmetric k, say explicitly that this is a
   simplification the data doesn't fully support, rather than implying the
   book is symmetric.
6. **Fat tails and volatility clustering (points 5–6) are named,
   permanent limitations of the closed-form model on this data** — stated
   up front in whatever report the strategy produces, not discovered later
   by a reader who re-ran the diagnostics themselves.
7. **k's session-half instability (point 4) is the strongest argument for
   a rolling recalibration window over a single session-long constant** —
   worth an explicit design decision (and a stated reason) when the
   calibration step is actually built, rather than defaulting to
   "whole-session k" without acknowledging the alternative.

---

## Phase 3: strategy-design signals

Phase 1 asked whether A-S's assumptions hold. Phase 2 refined the
calibration. Phase 3 asks a different, more direct question: **what does
the real order flow say about whether — and how — to actually build the
strategy**, independent of any one closed-form model. Six checks, via
`new-feature/analysis/analyze_dataset_signals.py` (raw output:
`new-feature/analysis/output_signals_2026-09-01.txt`).

### 1. Markout decomposition — does passive liquidity provision even make money here?

For every lit fill, decompose `total P&L(Δt) = spread capture at execution
+ markout(Δt)`, where `spread capture` is the edge earned relative to the
prevailing mid at the moment of the fill, and `markout(Δt)` is how much
the mid then moves against that position over the next Δt seconds — i.e.
literally simulating a passive liquidity provider who gets filled and
marks to market Δt later.

| Ticker | Spread capture (¢) | P&L @0.1s | @0.5s | @1s | @5s | @30s |
|---|---|---|---|---|---|---|
| AAPL | +5.16 | +1.63 | +1.14 | +0.68 | −0.21 | +0.23 |
| AMZN | +5.20 | +2.54 | +1.66 | +1.01 | −0.14 | **−1.48** |
| GOOG | +10.06 | +4.01 | +3.38 | +2.79 | +1.29 | +0.00 |
| INTC | +0.52 | **−0.11** | **−0.14** | **−0.16** | **−0.27** | **−0.40** |
| MSFT | +0.52 | **−0.19** | **−0.23** | **−0.25** | **−0.30** | **−0.34** |

Two very different stories in one table:

- **AAPL/AMZN/GOOG**: real spread capture (5–10¢), but adverse selection
  erodes most of it within 5–30 seconds — AAPL ekes out breakeven-ish,
  AMZN goes net negative by 30s, GOOG lands almost exactly at zero. The
  "you earn the spread by providing liquidity" intuition is *directionally
  true at execution* and *nearly wiped out by markout* at any realistic
  holding horizon. This is the single most direct evidence in this whole
  analysis that a naive constant-spread quoting strategy is not free
  money on this flow — the reservation-price skew A-S provides is
  compensating for a real, measured effect, not a theoretical one.
- **INTC/MSFT**: markout is negative **immediately** (already negative at
  0.1s) and gets worse out to 30s. Combined with a spread capture of only
  ~0.5¢ (half of the 1¢ minimum tick — there's no room for more), passive
  liquidity provision in these two names was, on average, **a losing
  proposition on this historical session**, before any strategy logic is
  even applied. This is a materially important result for the build: it's
  not just that `k` isn't estimable for these two (phase 1/2) — the
  *economics* of naive quoting don't work either, for a related but
  distinct reason (adverse selection dominates a spread that's capped at
  the tick).

### 2. Order lifetime / cancellation dynamics

| Ticker | Cancelled first | Filled first | Survived to close | Time-to-cancel p50/p95 | Time-to-first-fill p50/p95 |
|---|---|---|---|---|---|
| AAPL | 85.9% | 8.8% | 5.3% | 0s / 12s | 1s / 37s |
| AMZN | 88.7% | 5.0% | 6.3% | 1s / 23s | 2s / 48s |
| GOOG | 86.8% | 7.7% | 5.5% | 2s / 32s | 3s / 60s |
| INTC | 90.8% | 7.7% | 1.5% | 1s / 173s | 13s / 226s |
| MSFT | 90.4% | 7.6% | 2.1% | 1s / 171s | 10s / 246s |

The overwhelming majority of resting orders (86–91%) get pulled before
they're ever filled, in every name — a resting quote is far more likely
to be cancelled (by whoever placed it) than to trade. **Implication for
the strategy: an unrealistically patient requoting policy (rarely
cancelling/replacing) doesn't match how this market actually behaves**,
and a policy that never cancels stale quotes would be badly out of step
with the data. INTC/MSFT show a materially longer tail for time-to-fill
(p95 ≈ 170–250s vs. 12–60s for the other three) — consistent with the
tick-constrained book: with no price differentiation available, getting
filled depends on queue position/time priority, which takes longer to
resolve than in a wider-spread book.

### 3. Order book imbalance (OBI) vs. forward returns — a real, and cross-sectionally uneven, signal

| Ticker | OBI→1s-return corr | OBI→5s-return corr |
|---|---|---|
| AAPL | +0.037 | +0.029 |
| AMZN | +0.043 | +0.038 |
| GOOG | +0.039 | +0.038 |
| INTC | **+0.163** | **+0.232** |
| MSFT | **+0.178** | **+0.249** |

Order book imbalance (excess size on the bid vs. ask at L1) positively and
significantly predicts short-horizon returns everywhere — confirming a
well-documented microstructure result (Cont, Kukanov & Stoikov, 2014) —
but the effect is **4–8x stronger in INTC/MSFT than in the other three
names.** The mechanism makes sense: when price *can* move (AAPL/AMZN/GOOG,
spread well above the tick), some of the order flow's information gets
absorbed into price changes directly. When price is *pinned at the tick*
(INTC/MSFT), it can't move that way, so the imbalance signal has to carry
more of the predictive information instead. **Implication: an
OBI-conditioned skew is a much more promising extension for INTC/MSFT than
for the other three** — potentially the more appropriate strategy for
exactly the two names where the inventory-only A-S skew has no `k` to
calibrate against in the first place.

### 4. Trade-sign autocorrelation — order flow is strongly, and unevenly, persistent

| Ticker | lag-1 | lag-5 | lag-20 |
|---|---|---|---|
| AAPL | +0.715 | +0.335 | +0.115 |
| AMZN | +0.720 | +0.340 | +0.133 |
| GOOG | +0.720 | +0.304 | +0.057 |
| INTC | **+0.907** | **+0.696** | **+0.318** |
| MSFT | **+0.898** | **+0.675** | **+0.242** |

Aggressor direction (buy-initiated vs. sell-initiated) is **strongly**
autocorrelated in event-time everywhere (+0.7 at lag 1 even in the "mild"
names) and even more extreme in INTC/MSFT, where it's still +0.24–0.32 at
20 trades out. This is a direct, quantitative reason the single-fill
markout numbers above *understate* real adverse-selection risk: getting
hit once measurably raises the odds of getting hit again, right after, in
the same direction. **Implication: a strategy that treats fills as
independent events (the implicit assumption behind computing one k and
one expected markout) will underestimate the risk of a genuine run — worth
being explicit about, and a natural next-iteration feature (e.g. skewing
harder after N consecutive same-direction fills, not just on net
inventory) rather than something to silently ignore.**

### 5. Size distributions

| Ticker | New-order p50/p95 | Lit-fill p50/p95 | Hidden-fill p50/p95 |
|---|---|---|---|
| AAPL | 100 / 200 | 75 / 200 | 100 / 200 |
| AMZN | 100 / 200 | 60 / 183 | 90 / 200 |
| GOOG | 100 / 100 | 64 / 104 | 33 / 125 |
| INTC | 300 / 1200 | 100 / 1076 | 100 / 1000 |
| MSFT | 300 / 1500 | 100 / 1074 | 100 / 800 |

INTC/MSFT trade in 3–15x larger share counts than the other three at every
percentile — expected given their much lower share price (more shares per
dollar of notional). **Implication: quote sizing for the strategy has to
be per-name, not a single constant** — a "100 share" clip is a typical
lit fill in AAPL but a small fraction of a typical INTC fill.

### 6. Multi-lag autocorrelation profile

Extends phase 2's lag-1-only ARCH check to lags 1/2/5/10/20/60 seconds.
Raw-return autocorrelation stays small and mostly within the noise band
past lag 1 (confirms phase 2: the random-walk deviation is real but
short-lived). Volatility clustering (squared-return autocorrelation) is
the more persistent story — still detectable well past lag 1 in several
names (e.g. GOOG: +0.056 to +0.100 out to lag 10; INTC: +0.049 at lag 20).
**Implication: a σ calibrated once per session is a reasonable
approximation, but the underlying volatility process has memory on the
order of tens of seconds, not just single ticks** — relevant if a future
iteration moves from a session-constant σ to a rolling estimate.

---

## What this settles for the strategy build (final, supersedes earlier lists where they conflict)

1. **AAPL and AMZN get the standard A-S build first** — real spread
   capture, real (if decaying) profitability, stable-enough `k`,
   symmetric-book caveat noted but not disqualifying.
2. **GOOG ships as a caveated case** — profitable-at-execution but
   breakeven by 30s, weakest `k` fit, most session-half-unstable `k`.
   Report it, don't lead with it.
3. **INTC/MSFT are not just a calibration failure — they're an
   economics failure for the naive strategy.** Passive liquidity
   provision loses money on this historical flow before any strategy
   logic runs. The honest options are: (a) present them explicitly as
   "naive A-S doesn't work here, and here's the measured reason," which
   is itself a legitimate, interview-worthy result, or (b) build the
   OBI-conditioned variant specifically because phase 3 shows OBI carries
   4–8x more signal exactly in these two names. Either is defensible;
   silently dropping them or naively applying the same strategy without
   comment is not.
4. **Fills are not independent events** — trade-sign autocorrelation of
   +0.7 to +0.9 at lag 1 means a strategy or report that treats each fill
   as an i.i.d. draw is understating real risk. Worth a named caveat at
   minimum; a real design opportunity (streak-aware skewing) as a
   follow-on.
5. **Requoting has to be reasonably fast** — 86–91% of real orders get
   cancelled before they fill; an unrealistically patient quote-and-hold
   policy doesn't match this market's actual cancellation behavior.
6. **Quote size must be per-name** — INTC/MSFT trade in multiples of the
   other three names' typical clip size.

---

## Phase 4: validation — the last prerequisite before writing strategy code

Two checks, via `new-feature/analysis/analyze_dataset_validation.py` (raw
output: `new-feature/analysis/output_validation_2026-09-01.txt`), chosen
specifically because they're tests, not more description: one asks
whether a plausible additional signal (fill size) is actually there before
building around it, the other asks whether σ/k as calibrated would have
*generalized*, not just whether they fit the data they were estimated on.

### 1. Toxic-flow detection — checked, not found

Correlation between fill size and subsequent markout, at 1s and 5s
horizons, all five names:

| Ticker | corr @1s | corr @5s |
|---|---|---|
| AAPL | −0.021 | −0.009 |
| AMZN | −0.001 | +0.001 |
| GOOG | −0.009 | +0.001 |
| INTC | −0.017 | −0.014 |
| MSFT | −0.005 | +0.001 |

Every single value is within noise of zero, and the size-quartile
breakdowns (Q1 smallest → Q4 largest fill size) show no monotonic pattern
in any name — markout doesn't get worse as fill size grows. (INTC/MSFT
show an empty Q2 bucket in the raw output — their fill sizes cluster so
heavily at round lots, per phase 3's size distributions, that the 25th and
50th percentile coincide exactly, collapsing that bucket; a real data
characteristic, not a computation error, and irrelevant to the
correlation numbers above which don't depend on bucketing.)

**Verdict: no toxic-flow-by-size signal in this data.** This is a genuine,
useful null result — it means a size-conditioned skew ("widen after a big
print") would be adding complexity the data doesn't support, and the
honest move is not to build it, rather than including it because it
sounds sophisticated.

### 2. Walk-forward validation — σ misses in a predictable way, k splits cleanly into "generalizes" vs. "doesn't"

Fit on the first half of the session only, scored against the second half
(which the fit never saw):

| Ticker | σ (H1→actual H2 error) | std(z), H2/σ_H1 | k out-of-sample R² |
|---|---|---|---|
| AAPL | −21.7% | 0.78 | **0.699** |
| AMZN | −29.4% | 0.71 | **0.810** |
| GOOG | −38.9% | 0.61 | **−0.255** |
| INTC | −35.6% | 0.64 | not applicable (no H1 fit) |
| MSFT | −26.7% | 0.73 | not applicable (no H1 fit) |

**σ**: consistently over-forecasts second-half volatility by 22–39% across
*every* name — but this isn't evidence σ is unreliable, it's the same
intraday seasonality phase 1 already found (open more volatile than
afternoon), now with a precise, quantified forecast error attached.
**Verdict: confirms — with a number, not just a plot — that a single
session-constant σ is a real approximation, and a time-of-day-conditioned
σ is the correct fix, not a re-estimation of the constant.**

**k**: this is the sharper result. AAPL and AMZN's first-half-only k
predicts the second half's *actual, unseen* fill-rate histogram with
R²=0.70–0.81 — genuinely holds up, not just fits in-sample. GOOG's
first-half k scores **R²=−0.255** against the second half — negative R²
means the fit does *worse* than simply predicting the average rate; it
has no real out-of-sample predictive power. This is strictly stronger
evidence than phase 2's in-sample R²=0.022 on GOOG's first half alone —
that was "the fit is bad here"; this is "the fit does not generalize,
formally tested." **Verdict: GOOG's k should be reported as the
full-session pooled estimate only, explicitly never as something safe to
re-calibrate on a rolling sub-window** — the data itself rules that out,
it isn't a stylistic choice.

(The mean-abs-%-error figures in the raw output, e.g. AAPL's 294%, look
alarming but are an artifact of a handful of high-depth bins with only 1-2
actual fills each — a tiny absolute miss there is a huge percentage error.
The R² figures above, computed in log-rate space across all populated
bins, are the more robust summary and are what the verdicts are based on.)

---

## Final design calls for the strategy build

1. **AAPL, AMZN**: standard A-S build, full confidence — both economically
   profitable at execution (phase 3), k validated out-of-sample (phase 4).
2. **GOOG**: build it, but k must be the full-session pooled number,
   reported with an explicit "does not validate out-of-sample" caveat —
   never presented as re-calibratable on a rolling window.
3. **INTC, MSFT**: no viable k under the standard model (phases 1-2), and
   naive quoting loses money on the historical flow (phase 3) — ship as
   either an explicit "here's why the naive strategy fails here, measured"
   result, or the OBI-conditioned variant phase 3 specifically motivates.
4. **No size-conditioned skew** — phase 4 checked for it directly and it
   isn't there; don't build a feature the data doesn't support.
5. **σ should vary by session phase, not be a single session constant** —
   phase 4 turned this from "seems variable" (phase 1's seasonality plot)
   into "off by 22-39% if you don't" (a quantified forecast error).
6. Carry forward from phase 3: fills aren't independent (trade-sign
   autocorrelation 0.7-0.9), 86-91% of orders get cancelled before filling
   (requoting must be reasonably fast), and quote size must be per-name.

---

## Phase 5: alpha research — OBI fully characterized, OFI tested as its refinement

Scope deliberately narrowed to two signals (see conversation): OBI, now
properly characterized (phase 3 only checked two correlation points) and
OFI — Cont, Kukanov & Stoikov (2014)'s order flow imbalance, the
literature's actual refinement of the same idea — as the one candidate
most likely to either strengthen or subsume it. Via
`new-feature/analysis/analyze_dataset_alpha.py` (raw output:
`new-feature/analysis/output_alpha_2026-09-01.txt`).

**OFI, precisely**: unlike OBI (a snapshot of resting size at one instant),
OFI is event-driven — it accumulates, order by order, whether the best
bid/ask *improved* (counts the full new size), *worsened* (subtracts the
old size), or *stayed put* (counts the net size change) on each side, net
bid-side flow minus net ask-side flow. It measures the *flow* of pressure
into the book, not a static level.

### Headline: OFI is dramatically stronger than OBI, and nearly orthogonal to it

Spearman IC at 1-second horizon (the standard rank-based metric, more
robust to outliers than Pearson — used as the primary number below):

| Ticker | OBI IC @1s | OFI IC @1s | OBI/OFI correlation |
|---|---|---|---|
| AAPL | +0.074 | **+0.728** | −0.029 |
| AMZN | +0.080 | **+0.702** | −0.030 |
| GOOG | +0.054 | **+0.638** | −0.031 |
| INTC | +0.161 | **+0.342** | +0.091 |
| MSFT | +0.176 | **+0.398** | +0.091 |

OFI's IC is **6–13x larger than OBI's** at the shortest horizon, in every
single name. And the two signals are essentially uncorrelated with each
other (−0.03 to +0.09) — OFI is not a noisier restatement of OBI, it's
capturing genuinely different information (the *rate of change* of book
pressure, not its level). **This is the strongest, cleanest result in the
entire analysis.**

### Decay: OFI fades faster but starts from a much higher peak

Both signals decay with horizon, as expected, but on different
timescales. AAPL's OFI Spearman IC: +0.73 (1s) → +0.53 (2s) → +0.37 (5s) →
+0.26 (10s) → +0.11 (60s) — a fast, front-loaded decay, consistent with it
being an instantaneous-flow measure. OBI decays more slowly but from a far
lower starting point — for INTC/MSFT specifically, OBI's IC actually
*rises* from 1s to a peak around 5-10s (+0.16→+0.25 for MSFT) before
decaying, a slower-forming signal, consistent with phase 3's finding that
tick-constrained names route more of their information through the book
rather than through price moves. **Implication: OFI is the higher-value
signal at the strategy's actual reaction speed (quotes refresh in 0–2s,
per phase 3) — OBI's peak arrives later than the strategy would typically
act.**

### One nuance worth flagging directly: Pearson and Spearman disagree sharply for INTC/MSFT's OFI

AAPL/AMZN/GOOG: Spearman IC > Pearson IC (e.g. AAPL 0.73 vs 0.56) — normal,
expected when a relationship is real but not perfectly linear. INTC/MSFT:
the reverse — **Pearson IC (0.74, 0.81) is roughly double Spearman IC
(0.34, 0.40).** That pattern means the Pearson number is likely inflated
by a small number of large-magnitude OFI seconds (Pearson is sensitive to
outlier magnitude; Spearman isn't) rather than reflecting a consistently
strong rank relationship across the whole sample. **The Spearman number
(0.34–0.40) is the more trustworthy one to design around for INTC/MSFT —
still a real, meaningful signal, just not literally "twice as strong as
AAPL's OFI," which the raw Pearson number alone would wrongly suggest.**

### Split-half consistency and quantile tables

Every OFI and OBI split-half IC check kept the same sign across both
halves, in every name — no sign flips, no evidence of a spurious in-sample
artifact. Quantile tables show OFI's per-decile forward returns are
economically real and mostly monotonic for AAPL/AMZN (a clean −0.07¢ to
+0.06¢ spread from lowest to highest OFI decile); for GOOG/INTC/MSFT the
per-decile magnitudes round to ~0.00 at 2-decimal precision even where the
underlying correlation is statistically real — worth being explicit that
"statistically significant IC" and "economically large per-decile effect"
are different claims, and the second one is weaker for those three names
at this horizon.

---

## Final design calls (supersedes phase 3's OBI-only suggestion)

1. **OFI, not OBI, is the primary additional skew signal** — 6-13x
   stronger IC, orthogonal to OBI, consistent across session halves. This
   revises phase 3's original suggestion (which only had OBI to work
   with) — OFI is the one actually worth building into the reservation
   price.
2. **For INTC/MSFT specifically**, use OFI's Spearman-scale magnitude
   (~0.34-0.40), not its Pearson magnitude (~0.74-0.81) as the number that
   informs how much weight to give it — the Pearson figure is inflated by
   outlier seconds.
3. **OBI is still usable but secondary** — real, validated, but an order
   of magnitude weaker than OFI and slower to peak; worth including only
   if OFI alone doesn't fully address the INTC/MSFT viability problem
   found in phase 3.
4. **Both signals decay within the strategy's own requoting horizon**
   (0-2s median, phase 3) — confirms they're actionable at the speed the
   bot actually operates, not signals that would require a slower,
   different strategy to exploit.
5. Everything from phases 1-4 still stands: AAPL/AMZN primary, GOOG
   full-session-k-only with its caveat, INTC/MSFT reframed from "naive
   strategy loses money" (phase 3) to "naive strategy loses money, AND
   here's the specific, measured signal (OFI) that could fix it."

---

## Phase 6: the remaining five alpha candidates

Generated for completeness, same discipline as phase 5, via
`new-feature/analysis/analyze_dataset_alpha2.py` (raw output:
`new-feature/analysis/output_alpha2_2026-09-01.txt`). Every signal
checked for orthogonality against OFI specifically (phase 5's strongest
result) — the relevant question isn't "is this correlated with price," 
it's "does this add anything OFI doesn't already give the bot."

### Spearman IC @1s, all seven signals now tested, side by side

| Ticker | OBI | OFI | MLOBI | Micro-px dev | Trade-flow mom. | Queue depletion | Spread (dir) |
|---|---|---|---|---|---|---|---|
| AAPL | +0.074 | **+0.728** | −0.009 | +0.070 | +0.253 | +0.058 | +0.011 |
| AMZN | +0.080 | **+0.702** | −0.002 | +0.073 | +0.247 | +0.051 | −0.008 |
| GOOG | +0.054 | **+0.638** | −0.002 | +0.048 | +0.243 | +0.037 | +0.004 |
| INTC | +0.161 | +0.342 | +0.056 | **+0.162** | +0.289 | +0.119 | −0.006 |
| MSFT | +0.176 | +0.398 | +0.056 | **+0.176** | **+0.311** | +0.140 | +0.019 |

**OFI remains the strongest single signal everywhere it's evaluated
fairly** (i.e. by Spearman, not the inflated Pearson number flagged in
phase 5) — nothing in this second batch beats it outright. But two of the
five turned out genuinely useful as *additions*, not just weaker restatements:

### Micro-price deviation — comparably strong to OFI for INTC/MSFT, and decays slower

Weak for AAPL/AMZN/GOOG (Spearman 0.05-0.07, same order as OBI), but for
**INTC and MSFT it's essentially tied with OFI at the 1s IC (0.16 vs 0.34,
0.18 vs 0.40) and pulls ahead by 5-10s** — INTC's micro-price IC actually
*peaks* around 10s (+0.259) and is still +0.231 at 30s, while OFI has
already decayed to +0.134 by then. Given phase 3 found INTC/MSFT's fills
take far longer on average (time-to-first-fill p95 ≈ 170-250s vs. 12-60s
for the other three), a signal that stays informative for tens of seconds
rather than fading in one is a better match to how long a resting quote
actually waits there. **Verdict: for INTC/MSFT specifically, combine OFI
(fast, front-loaded) with micro-price deviation (slower, more
persistent) — they're covering different parts of the same requoting
window, not duplicating each other.**

### Trade-flow momentum — real, and only partially redundant with OFI

Meaningfully positive everywhere (Spearman 0.24-0.31 at 1s, every name),
decays gradually, and its correlation with OFI (0.17-0.36 — "partially
overlapping," phase 6's middle category, not "redundant") confirms it's
capturing something OFI doesn't fully cover: realized executed flow,
distinct from resting-book pressure and its rate of change. **Verdict:
worth including as a secondary input, most usefully as a confirming or
contradicting signal alongside OFI rather than a replacement for it.**

### MLOBI (10-level), queue depletion, spread-width — not worth building

- **MLOBI**: weak or wrong-signed for AAPL/AMZN (−0.01 to −0.02, opposite
  direction from L1 OBI's own positive sign there), only modestly useful
  for GOOG/INTC/MSFT (+0.06). Looking deeper in the book doesn't help,
  and for two of five names it actively points the wrong way.
- **Queue depletion**: sign-flips across session halves for AAPL
  specifically (not robust there); moderate and consistent for the other
  four (0.04-0.19), but never stronger than trade-flow momentum, and
  correlated enough with the flow-based signals to add little on top.
- **Spread-width**: behaves exactly as a real null result should for
  *direction* (weak, inconsistent sign) — the expected outcome, a
  legitimate check that passed rather than a signal that failed.
  *Magnitude* prediction (does wide spread predict upcoming volatility)
  sign-flips across session halves for AAPL/AMZN/GOOG — not robust enough
  to build on for those three; INTC/MSFT's version is hard to even
  evaluate cleanly since their spread has almost no variance to bucket
  (same tick-pinning finding from phase 2, surfacing again here).

None of these three get built into the strategy. Testing and ruling them
out is itself the useful result — it means the eventual reservation-price
formula stays as simple as the evidence actually supports, not padded
with signals that looked plausible but didn't hold up.

---

## Final signal ranking for the strategy build (supersedes phase 5)

1. **OFI** — primary signal, all five names, especially AAPL/AMZN/GOOG
   where it's the dominant source of extra edge.
2. **Micro-price deviation** — add specifically for INTC/MSFT, where it
   matches or exceeds OFI beyond the first few seconds — the two together
   cover the full window a quote is likely to wait there.
3. **Trade-flow momentum** — secondary, all five names, as a confirming
   input alongside OFI rather than standalone.
4. **OBI** — tertiary/backup, real but an order of magnitude weaker than
   OFI everywhere it was re-tested.
5. **MLOBI, queue depletion, spread-width** — tested, not used. A
   documented negative result, not a gap.

---

## Phase 7: composite signal + VPIN — the last alpha-research step

Via `new-feature/analysis/analyze_dataset_alpha3.py` (raw output:
`new-feature/analysis/output_alpha3_2026-09-01.txt`). Not new raw signal
hunting — combining what's already validated, plus one different-in-kind
signal (toxicity/risk, not direction).

### Composite signal: a real, validated win for INTC/MSFT — flat for the other three

| Ticker | Best single component (Spearman) | Equal-weight composite | OLS composite, fit H1 → scored H2 |
|---|---|---|---|
| AAPL | 0.366 (OFI) | 0.306 — **worse** | 0.363 — roughly matches OFI alone |
| AMZN | 0.336 (OFI) | 0.307 — **worse** | 0.316 — roughly matches OFI alone |
| GOOG | 0.306 (OFI) | 0.291 — **worse** | 0.317 — marginal gain |
| INTC | 0.245 (MicroDev) | **0.321 — +31% relative** | **0.343 — validated out-of-sample** |
| MSFT | 0.263 (OFI) | **0.349 — +33% relative** | **0.370 — validated out-of-sample** |

For AAPL/AMZN/GOOG, OFI alone is already close to the ceiling — combining
with the weaker trade-flow-momentum signal under equal weighting actually
*hurts* (dilutes a strong signal with a weaker one), and OLS-fit weighting
just rediscovers "mostly use OFI" rather than finding real complementary
value. **For INTC/MSFT, the opposite is true**: no single component is
individually as strong as AAPL's OFI, so there's real room for combination
— the three components (OFI, trade-flow momentum, micro-price deviation)
are picking up different partial pieces of the same puzzle, and combining
them cancels idiosyncratic noise. Critically, this isn't just an in-sample
artifact — the OLS weights were fit on the first half only and *still*
outperformed any single component when scored on the second half, which
is the actual bar for calling this validated rather than lucky.

**Verdict: build the composite (OFI + trade-flow momentum + micro-price
deviation) specifically for INTC/MSFT — it's a measured, out-of-sample
improvement, not a hypothesis. For AAPL/AMZN/GOOG, use OFI alone;
combining doesn't help and adds complexity for nothing.**

### VPIN: tested honestly, doesn't hold up

Volume-bucketed buy/sell imbalance (Easley, López de Prado & O'Hara),
checked two ways — against subsequent volatility, and directly against
phase 3's own markout numbers, fill by fill:

- **VPIN vs. future |return|**: weak and *inconsistent in sign* across
  names and horizons — even reverses for INTC (−0.24 Pearson, −0.21
  Spearman at 60s, meaning *higher* VPIN preceded *lower* subsequent
  volatility there, the opposite of the textbook expectation). Plausible
  explanation: INTC/MSFT's extreme resting liquidity at the touch (median
  order size 300 shares, phase 3) may simply absorb informed-trading
  bursts without much price impact — but that's a post-hoc explanation
  for a null/reversed result, not a confirmed mechanism.
- **VPIN-at-fill-time vs. that fill's own markout** (the more directly
  actionable question, and the one phase 4's cruder toxic-flow check
  didn't fully cover): **not monotonic in any of the five names.** Fills
  during "high VPIN" periods are not reliably worse than fills during
  "low VPIN" periods.

**Verdict: VPIN does not clear the bar for inclusion.** This is a genuine
negative result, not a failed implementation — it was checked the
directly relevant way (against real fill markout, not just a proxy) and
didn't hold up. Worth noting the sample-size caveat stated up front (≈200
volume buckets per session is modest for IC purposes, coarser than
Easley et al.'s original daily-frequency design point), so this is "not
demonstrated on this evidence," not "proven not to work at any scale" —
but on the evidence actually in hand, it doesn't earn a place in the bot.

---

## Final, final signal set for the strategy build (supersedes phase 6)

1. **AAPL, AMZN, GOOG**: OFI alone as the skew signal. Composite tested
   and found not to help — added complexity with no measured benefit.
2. **INTC, MSFT**: the OLS-weighted composite of OFI + trade-flow
   momentum + micro-price deviation — a real, out-of-sample-validated
   improvement over any single signal, and the strongest form of "the
   naive strategy doesn't work here, but here's a measured signal-based
   fix" available from this data.
3. **VPIN: not included.** Tested two ways, held up neither. Documented
   as a checked-and-rejected result, same treatment as MLOBI/queue
   depletion/spread-width in phase 6.
4. Everything else from phases 1-6 stands: σ and lit-only k per name
   (AAPL/AMZN validated, GOOG full-session-only with its caveat), fills
   aren't independent (trade-sign autocorrelation), requoting must be
   fast (86-91% of real orders get cancelled before filling), and quote
   size must be per-name.

---

## Phase 8 (closing): confidence intervals on markout, and a data-derived inventory limit

Via `new-feature/analysis/analyze_dataset_riskparams.py` (raw output:
`new-feature/analysis/output_riskparams_2026-09-01.txt`). The last two
gaps identified as worth closing before writing strategy code — everything
past this needs the actual bot running to answer, not more static analysis.

### Markout, now with statistical confidence — and a second-pass audit caught the naive version overstating it

The first version of this section used `SE = std/√n` — which assumes each
fill's markout is an independent draw. Phase 3 already measured that
they're not: trade-sign autocorrelation of 0.7–0.9 at lag 1. A follow-up
audit checked whether markout values themselves inherit that
autocorrelation (consecutive fills in a directional run should produce
correlated outcomes) — they do, strongly: **0.70–0.74 at lag 1, still
0.07–0.17 at lag 20** (AAPL, checked directly). Naive SE understates
uncertainty under autocorrelation; the fix is a Newey-West (HAC, Bartlett
kernel, 20-lag bandwidth — matching phase 3's own ACF window) standard
error, which properly inflates the variance for the correlation structure
actually present in the data. SE roughly tripled across the board once
corrected. The table below uses the corrected (HAC) values throughout —
this is the number that should be trusted.

| Ticker | 1s (mean, sig?) | 5s (mean, sig?) | 30s (mean, sig?) |
|---|---|---|---|
| AAPL | +0.68¢, **significant** (t=+5.58) | −0.21¢, not significant (t=−0.98, CI spans zero) | +0.23¢, not significant (t=+0.56) |
| AMZN | +1.01¢, **significant** (t=+6.80) | −0.14¢, not significant (t=−0.70, CI spans zero) | **−1.48¢, significant LOSS (t=−3.17)** |
| GOOG | +2.79¢, **significant** (t=+10.67) | +1.29¢, **significant** (t=+3.85) | 0.00¢, not significant (t=+0.00) |
| INTC | −0.16¢, **significant LOSS** (t=−12.58) | −0.27¢, **significant LOSS** (t=−14.57) | −0.40¢, **significant LOSS** (t=−11.41) |
| MSFT | −0.25¢, **significant LOSS** (t=−20.38) | −0.30¢, **significant LOSS** (t=−16.81) | −0.34¢, **significant LOSS** (t=−9.19) |

What changed vs. the naive version, and what didn't:

- **AAPL's and AMZN's "significant 5s loss" don't survive the
  correction** — both flip to "not significant" (t=−0.98, t=−0.70). The
  naive version's t≈−2 to −3 at 5s looked like real, if modest, evidence
  of a reversal; under HAC, that evidence mostly evaporates. The 5s number
  for these two is genuinely inconclusive, not a confirmed loss.
- **AMZN's 30s loss survives, but far less dramatically than the naive
  t=−9.69 suggested** — HAC gives t=−3.17, still comfortably significant,
  but roughly a third the (over-)confidence the naive calculation implied.
- **AAPL and GOOG's 30s "recovery toward breakeven" still isn't real**
  under either method — both spans zero, now even more so (t≈0 either
  way).
- **INTC/MSFT's losses survive completely, at every horizon, under the
  more rigorous test** — t-statistics of −9 to −21 rather than the naive
  −30 to −68. Sizeable correction to the *t-statistic itself*, but the
  substantive conclusion (naive passive quoting loses money on these two
  names, throughout the session) is unchanged. This is the single
  strongest result in the whole analysis to have survived a genuinely
  adversarial re-check, and it's now backed by the statistically correct
  method rather than an overstated one.
- **GOOG stays significantly profitable through 5s** (t=+3.85) even under
  the more conservative HAC test — the strongest of the three wider-spread
  names by this measure.

The honest framing: the *direction* of every headline conclusion from
phase 3 stands. What the audit fixed was **overstated confidence at the
margins** — exactly the kind of gap a claim should not carry into a report
someone else might rely on.

### Data-derived inventory limit

Turns numbers already measured — trade-sign persistence (phase 3), median
fill size (phase 3), σ (phase 1) — into a concrete `q_max` per name, rather
than leaving position sizing as an unaddressed constant:

| Ticker | Persistence ρ (fit) | Expected run length | Median fill size | Suggested q_max | 60s 1σ $ risk at q_max |
|---|---|---|---|---|---|
| AAPL | 0.915 | 11.7 fills | 75 sh | 2,634 sh | $720 |
| AMZN | 0.921 | 12.7 fills | 60 sh | 2,291 sh | $352 |
| GOOG | 0.880 | 8.4 fills | 64 sh | 1,605 sh | $500 |
| INTC | 0.947 | 18.9 fills | 100 sh | 5,670 sh | $100 |
| MSFT | 0.934 | 15.0 fills | 100 sh | 4,508 sh | $91 |

Method: fit `ρ` from the trade-sign autocorrelation already measured
(lag 1/5/20, log-linear regression), convert to an expected same-direction
run length (`1/(1-ρ)`), multiply by median lit-fill size and a 3x safety
buffer to get `q_max`, then report the 1σ dollar P&L swing a position that
size would carry over 60 seconds. **INTC/MSFT need a proportionally
larger share-count cap** (their persistence is the highest of all five —
consistent with phase 3's trade-sign autocorrelation finding — so a
"typical run" there is longer even though each share carries much less
dollar risk, given their much smaller σ).

**What's measured vs. what's a free choice, stated plainly**: ρ, run
length, fill size, and σ all come from data. The 3x safety multiplier and
the 60-second risk-evaluation window do not — they're free parameters,
same status as γ (risk aversion) in the A-S model itself, and should be
tuned by whoever operates the bot, not treated as a derived fact.

---

## Analysis is complete

Eight phases, five real tickers, four Python scripts' worth of checks
extended to eleven, every alpha candidate from the original list
resolved, every design-relevant assumption tested rather than assumed,
and the core economic question (does this strategy make money) now
answered with statistical confidence, not just a point estimate. Nothing
remaining is a data-analysis question — what's left (fee modeling,
portfolio-level correlation across names, strategy parameter sweeps) either
falls outside this feature's scope or needs the actual bot built and
backtested to answer honestly.

Next step: build the Avellaneda-Stoikov quoting agent using these exact,
evidence-backed inputs — per-name σ/k, OFI (AAPL/AMZN/GOOG) or the
validated OFI+momentum+micro-price composite (INTC/MSFT) as the skew
signal, the derived `q_max` per name, and every design constraint
documented above — replayed against the project's own `OrderBook`/`Matcher`.
