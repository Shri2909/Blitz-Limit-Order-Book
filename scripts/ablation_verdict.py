"""Ablation verdict rule ("separated and material" on per-trial P99.9).

Single Python implementation, imported by scripts/run_ablations.sh and by
tests/test_verdict_parity.py. It mirrors hydra::stats::classify_ablation() in
include/hydra/stats.hpp; tests/verdict_cases.json is run through both so drift fails a test.

Rule, first match wins (baseline and ablated each have one P99.9 per trial):
  INVALID               a run is missing or failed a validity gate, an arm has no trials,
                        or the baseline median P99.9 is <= 0
  INCONCLUSIVE          fewer than MIN_TRIALS trials in either arm
  NO_MEASURABLE_EFFECT  median ablated within +/-10% of the baseline median
  PROVEN                median ablated >= 1.10x baseline AND min(ablated) > max(baseline)
  ABLATED_FASTER        median ablated <= baseline / 1.10 AND max(ablated) < min(baseline)
  INCONCLUSIVE          a material median gap whose per-trial ranges overlap
Always state the metric with the verdict (METRIC), e.g. "PROVEN (P99.9)".
"""

from statistics import median

METRIC = "P99.9"
MIN_EFFECT_PERCENT = 10  # integer percent keeps the boundary comparison exact
MIN_TRIALS = 5

RULE_SUMMARY = (
    f"PROVEN = median ablated {METRIC} >= {100 + MIN_EFFECT_PERCENT}% of baseline and every "
    f"ablated trial above every baseline trial ({MIN_TRIALS} trials per arm); "
    f"NO_MEASURABLE_EFFECT = medians within +/-{MIN_EFFECT_PERCENT}%; "
    "ABLATED_FASTER = the mirror of PROVEN; INCONCLUSIVE = material gap but overlapping trials; "
    "INVALID = failed validity gate."
)

_GATES = ("valid", "determinism_ok")
_ZERO_COUNTS = ("dropped_events", "arena_fallback_count", "pool_exhaustion_count")


def _run_clean(run):
    if not run.get("ok"):
        return False
    if not all(run.get(k) for k in _GATES):
        return False
    return all(run.get(k) == 0 for k in _ZERO_COUNTS)


def classify(baseline, ablated, pct=MIN_EFFECT_PERCENT, min_trials=MIN_TRIALS):
    """Return the bare verdict code for two run dicts (as written to ablation_raw.jsonl)."""
    if baseline is None or ablated is None:
        return "INVALID"
    if not _run_clean(baseline) or not _run_clean(ablated):
        return "INVALID"
    b = baseline.get("p999_trials") or []
    a = ablated.get("p999_trials") or []
    if not b or not a or pct <= 0 or median(b) <= 0:
        return "INVALID"
    if len(b) < min_trials or len(a) < min_trials:
        return "INCONCLUSIVE"
    bm, am = median(b), median(a)
    up = 100 + pct
    worse = am * 100 >= bm * up
    better = am * up <= bm * 100
    if not worse and not better:
        return "NO_MEASURABLE_EFFECT"
    if worse and min(a) > max(b):
        return "PROVEN"
    if better and max(a) < min(b):
        return "ABLATED_FASTER"
    return "INCONCLUSIVE"


def verdict_text(code):
    """Verdict with the metric attached, e.g. 'PROVEN (P99.9)'; INVALID has no metric."""
    return code if code == "INVALID" else f"{code} ({METRIC})"
