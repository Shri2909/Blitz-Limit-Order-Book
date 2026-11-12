"""Regression coverage for chart status when CTest evidence is missing or failed."""

import importlib.util
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

    def test_hero_chart_renders_unavailable_status_without_a_log(self):
        evidence = self.root / "evidence"
        evidence.mkdir()
        (evidence / "price_time.csv").write_text(
            "p50_ns_per_order,p90_ns_per_order,p99_ns_per_order,p999_ns_per_order,"
            "throughput_orders_per_second,dropped_events\n"
            "100,200,300,400,1000,0\n"
        )
        image_path = self.root / "hero.png"
        with (
            patch.object(charts, "REPO_ROOT", self.root),
            patch.object(charts.plt, "close", wraps=charts.plt.close) as close,
        ):
            charts.chart_hero_stats(evidence, "fixture", image_path)
            figure = close.call_args.args[0]
            labels = [text.get_text() for axis in figure.axes for text in axis.texts]
        self.assertTrue(image_path.is_file())
        self.assertIn("N/A", labels)
        self.assertNotIn("10/10", labels)


if __name__ == "__main__":
    unittest.main()
