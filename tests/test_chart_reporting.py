"""Regression coverage for chart reporting: test status, verdict vocabulary, labels, determinism."""

import importlib.util
import json
import sys
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


chart_path = Path(__file__).resolve().parents[1] / "charts" / "generate_charts.py"
spec = importlib.util.spec_from_file_location("blitz_charts", chart_path)
charts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(charts)


class ChartReportingTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        self.root = Path(self.temp_dir.name)
        self.log = self.root / "LastTest.log"

    def test_missing_and_empty_logs_have_no_test_result(self):
        self.assertIsNone(charts.parse_ctest_results(self.log))
        self.log.write_text("Start testing\nEnd testing\n")
        self.assertIsNone(charts.parse_ctest_results(self.log))

    def test_recorded_results_include_failures_once(self):
        self.log.write_text("Test Passed.\nTest Failed.\nTest Passed.\n1/3 ...***Failed\n")
        self.assertEqual(charts.parse_ctest_results(self.log), (2, 3))

    def test_all_recorded_passes_are_counted(self):
        self.log.write_text("Test Passed.\nTest Passed.\n")
        self.assertEqual(charts.parse_ctest_results(self.log), (2, 2))

    def test_missing_executables_do_not_count_as_passes(self):
        self.log.write_text("Unable to find executable: /missing/test\nTest Passed.\n")
        self.assertIsNone(charts.parse_ctest_results(self.log))
        self.log.write_text('Command: ""\nTest Passed.\n')
        self.assertIsNone(charts.parse_ctest_results(self.log))

    def test_unavailable_status_is_not_a_success_claim(self):
        tile = charts.test_status_tile(None)
        self.assertEqual(tile[0], "N/A")
        self.assertIn("unavailable", tile[1])
        self.assertNotIn("clean", " ".join(tile))

    def test_failed_results_do_not_claim_sanitizer_success(self):
        tile = charts.test_status_tile((2, 3))
        self.assertEqual(tile[0], "2/3")
        self.assertNotIn("clean", " ".join(tile))

    def write_hero_fixture(self):
        evidence = self.root / "evidence"
        evidence.mkdir(exist_ok=True)
        (evidence / "price_time.csv").write_text(
            "commit_hash,p50_ns_per_order,p90_ns_per_order,p99_ns_per_order,p999_ns_per_order,"
            "throughput_orders_per_second,dropped_events\n"
            "abc1234,100,200,300,400,84991.5,0\n"
        )
        return evidence

    def render_hero(self, evidence, image_path, ctest_log=None):
        with patch.object(charts.plt, "close", wraps=charts.plt.close) as close:
            charts.chart_hero_stats(evidence, "abc1234", image_path, ctest_log)
            figure = close.call_args.args[0]
            return [text.get_text() for axis in figure.axes for text in axis.texts] + [
                text.get_text() for text in figure.texts
            ]

    def test_hero_chart_renders_unavailable_status_without_a_log(self):
        evidence = self.write_hero_fixture()
        image_path = self.root / "hero.png"
        labels = self.render_hero(evidence, image_path)
        self.assertTrue(image_path.is_file())
        self.assertIn("N/A", labels)
        self.assertNotIn("10/10", labels)

    def test_hero_chart_uses_log_and_honest_load_label(self):
        evidence = self.write_hero_fixture()
        self.log.write_text("Test Passed.\n" * 10)
        labels = self.render_hero(evidence, self.root / "hero.png", self.log)
        self.assertIn("10/10", labels)
        self.assertIn("85,000/s", labels)
        self.assertIn("offered load (paced replay)", labels)
        self.assertFalse([t for t in labels if "sustained" in t.lower()])
        self.assertTrue([t for t in labels if t.startswith("commit abc1234")])

    def test_hero_chart_is_byte_deterministic(self):
        evidence = self.write_hero_fixture()
        first, second = self.root / "a.png", self.root / "b.png"
        self.render_hero(evidence, first)
        self.render_hero(evidence, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_verdict_vocabulary_and_legacy_names(self):
        for name in ("PROVEN", "INCONCLUSIVE", "NO_MEASURABLE_EFFECT", "ABLATED_FASTER", "INVALID"):
            self.assertEqual(charts.normalize_verdict(name), name)
        self.assertEqual(charts.normalize_verdict("REGRESSION"), "ABLATED_FASTER")
        with self.assertRaises(ValueError):
            charts.normalize_verdict("NOT_PROVEN")

    def test_x_worse_is_claimed_only_for_proven(self):
        trials = [1219.0, 1317.0, 1452.0, 1615.0, 2293.0]
        proven = charts.ablated_label("PROVEN", 4165, 3.63, trials)
        self.assertIn("3.63x worse", proven[0])
        self.assertEqual(len(proven), 2)
        for verdict in ("INCONCLUSIVE", "NO_MEASURABLE_EFFECT", "ABLATED_FASTER"):
            lines = " ".join(charts.ablated_label(verdict, 1452, 1.24, trials))
            self.assertNotIn("worse", lines)
        self.assertIn(
            "no measurable effect", charts.ablated_label("NO_MEASURABLE_EFFECT", 1, 1, trials)[0]
        )
        self.assertIn("faster", charts.ablated_label("ABLATED_FASTER", 1, 0.8, trials)[0])
        inconclusive = charts.ablated_label("INCONCLUSIVE", 1452, 1.24, trials)
        self.assertIn("highly load dependent", inconclusive[1])
        self.assertIn("1,219\u20132,293", inconclusive[1])

    def test_fragile_proven_label_states_margin_and_load_dependence(self):
        baseline = [1157.0, 1129.0, 1143.0, 1148.0, 1155.0]
        trials = [52559.0, 69960.0, 2435.0, 1682.0, 1161.0]
        lines = charts.ablated_label("PROVEN", 2435, 2.12, trials, baseline)
        text = " ".join(lines)
        self.assertIn("2.12x worse", lines[0])
        self.assertIn("1,161\u201369,960", lines[1])
        self.assertIn("highly load dependent", text)
        self.assertIn("separation margin only 4 ns", text)
        self.assertIn("flips between runs", text)

    def test_committed_ablation_arms_render_with_expected_verdicts(self):
        results = charts.DEFAULT_RESULTS_DIR
        if not results.is_dir():
            self.skipTest("run-of-record results are not present locally")
        rows = [
            r
            for r in charts.read_csv_rows(results / "ablation_summary.csv")
            if r["matching_mode"] == "price_time"
        ]
        verdicts = {r["design_decision"]: charts.normalize_verdict(r["verdict"]) for r in rows}
        self.assertEqual(verdicts["Cache-line hot/cold split"], "NO_MEASURABLE_EFFECT")
        self.assertEqual(verdicts["Lock-free SPSC queue"], "PROVEN")
        self.assertIn("ABLATED_FASTER", charts.pro_rata_summary(results))

    def test_ablation_rows_cover_the_four_decisions(self):
        self.assertEqual(len(charts.DECISIONS), 4)
        titles = [title for _, title in charts.DECISIONS.values()]
        self.assertIn("Core isolation + pinning vs unpinned (shared desktop)", titles)

    def test_clip_cap_only_clips_outliers(self):
        self.assertEqual(charts.clip_cap([1148, 4165, 2435]), 4165)
        self.assertEqual(charts.clip_cap([1000, 1200, 30000]), 1500)

    def test_percentage_change_uses_signed_rounding(self):
        self.assertEqual(charts.percentage_change(485, 320), "\u221234%")
        self.assertEqual(charts.percentage_change(100, 125), "+25%")
        self.assertEqual(charts.percentage_change(100, 100), "0%")

    def test_layout_transcription_tiles_each_cache_line(self):
        for rects, start in ((charts.HOT_LINE_RECTS, 0), (charts.COLD_LINE_RECTS, 64)):
            cursor = start
            for _, offset, size, _ in rects:
                self.assertEqual(offset, cursor)
                cursor += size
            self.assertEqual(cursor, start + 64)

    def test_cli_options_and_full_run_are_deterministic(self):
        results = charts.DEFAULT_RESULTS_DIR
        baseline = charts.DEFAULT_BASELINE_DIR
        if not (results.is_dir() and baseline.is_dir()):
            self.skipTest("run-of-record results are not present locally")
        outputs = []
        for name in ("one", "two"):
            out = self.root / name
            argv = ["generate_charts.py", "--out-dir", str(out), "--ctest-log", str(self.log)]
            with patch.object(sys, "argv", argv):
                charts.main()
            outputs.append(out)
        names = sorted(p.name for p in outputs[0].iterdir())
        self.assertEqual(len(names), 5)
        for name in names:
            self.assertEqual((outputs[0] / name).read_bytes(), (outputs[1] / name).read_bytes())

    def _run_dir(self, name, isolated):
        run = self.root / name
        run.mkdir()
        if isolated is not None:
            snapshot = f"isolated={isolated} rx_core=4 matching_core=5"
            (run / "price_time.meta.json").write_text(json.dumps({"env_snapshot": snapshot}))
        return run

    def test_tuning_note_states_whether_the_hosts_were_tuned_alike(self):
        earlier = self._run_dir("earlier", "4-5")
        final = self._run_dir("final", "4-7")
        same = self._run_dir("same", "4-5")
        unknown = self._run_dir("unknown", None)
        self.assertIn("host tuning differed", charts.tuning_note(earlier, final))
        self.assertIn("4-5 earlier, 4-7 in the final run", charts.tuning_note(earlier, final))
        self.assertIn("same isolated cores (4-5)", charts.tuning_note(earlier, same))
        self.assertIn("not recorded", charts.tuning_note(earlier, unknown))


if __name__ == "__main__":
    unittest.main()
