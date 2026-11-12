"""Run tests/verdict_cases.json through scripts/ablation_verdict.py.

tests/test_phase10_verdict.cpp runs the same fixture through hydra::stats::classify_ablation(),
so a drift between the C++ rule and the Python rule used by scripts/run_ablations.sh fails one
of the two tests. Stdlib only: python3 -m unittest tests/test_verdict_parity.py
"""

import importlib.util
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "verdict_cases.json"

spec = importlib.util.spec_from_file_location("ablation_verdict", ROOT / "scripts" / "ablation_verdict.py")
verdict = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verdict)

# Fixture gate name -> run-dict field that run_ablations.sh writes into the raw JSONL.
GATE_FIELD = {
    "valid": ("valid", False),
    "determinism": ("determinism_ok", False),
    "dropped_events": ("dropped_events", 1),
    "arena_fallback": ("arena_fallback_count", 1),
    "pool_exhaustion": ("pool_exhaustion_count", 1),
}


def make_run(p999_trials, failed_gate=""):
    run = {
        "ok": True,
        "valid": True,
        "determinism_ok": True,
        "dropped_events": 0,
        "arena_fallback_count": 0,
        "pool_exhaustion_count": 0,
        "p999_trials": list(p999_trials),
    }
    if failed_gate:
        field, bad = GATE_FIELD[failed_gate]
        run[field] = bad
    return run


class VerdictParityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(FIXTURE) as f:
            cls.fixture = json.load(f)

    def test_constants_match_fixture(self):
        self.assertEqual(verdict.MIN_EFFECT_PERCENT, self.fixture["min_effect_percent"])
        self.assertEqual(verdict.MIN_TRIALS, self.fixture["min_trials"])

    def test_fixture_cases(self):
        self.assertGreater(len(self.fixture["cases"]), 0)
        for case in self.fixture["cases"]:
            with self.subTest(case=case["name"]):
                got = verdict.classify(
                    make_run(case["baseline"], case.get("baseline_fail", "")),
                    make_run(case["ablated"], case.get("ablated_fail", "")),
                )
                self.assertEqual(got, case["expected"])

    def test_every_verdict_is_exercised(self):
        seen = {c["expected"] for c in self.fixture["cases"]}
        self.assertEqual(
            seen,
            {"PROVEN", "ABLATED_FASTER", "NO_MEASURABLE_EFFECT", "INCONCLUSIVE", "INVALID"},
        )

    def test_missing_or_failed_run_is_invalid(self):
        good = make_run([100] * 5)
        self.assertEqual(verdict.classify(None, good), "INVALID")
        self.assertEqual(verdict.classify(good, None), "INVALID")
        failed = {"ok": False, "reason": "exit code 1"}
        self.assertEqual(verdict.classify(good, failed), "INVALID")
        self.assertEqual(verdict.classify(failed, good), "INVALID")
        # A raw record from before p999_trials existed cannot be classified.
        legacy = make_run([])
        del legacy["p999_trials"]
        self.assertEqual(verdict.classify(good, legacy), "INVALID")

    def test_verdict_text_names_the_metric(self):
        self.assertEqual(verdict.verdict_text("PROVEN"), "PROVEN (P99.9)")
        self.assertEqual(verdict.verdict_text("INVALID"), "INVALID")

    def test_shell_script_uses_the_shared_classifier(self):
        script = (ROOT / "scripts" / "run_ablations.sh").read_text()
        self.assertIn("from ablation_verdict import", script)
        self.assertIsNone(re.search(r"^def classify\(", script, re.MULTILINE))
        self.assertIn('"p999_trials"', script)


if __name__ == "__main__":
    unittest.main()
