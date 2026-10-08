import tempfile
import unittest
from pathlib import Path

from tools.run_modular_rma_regression import (
    last_gem5_tick, tick_delta, command, validate_depth_output, OUTSTANDING_CASES,
)


class DepthTests(unittest.TestCase):
    def transcript(self, depth=512):
        return "".join(f"--- UART {port} ---\nJFS depth : {depth}\n"
                       "4096 2048 0.00 12.34 0.001234\n"
                       for port in (3560, 3570))

    def test_explicit_depth_and_wrap_matrix(self):
        self.assertEqual(len(OUTSTANDING_CASES), 9)
        for _, verb, size, n, batch, cq, bidi, scan in OUTSTANDING_CASES:
            cmd = command(verb, size, n, batch, cq, 21300, None,
                          bidi, False, scan, 512)
            self.assertIn("-T 512 -R 512", cmd)
            self.assertGreaterEqual(n, 4 * 512)
            self.assertEqual(n % batch, 0)

    def test_complete_both_nodes(self):
        self.assertEqual(validate_depth_output(self.transcript(), 512, 4096, 2048), [])

    def test_reject_clamped_depth_missing_node_or_incomplete_row(self):
        for text in (self.transcript(128), "", self.transcript().split("--- UART 3570")[0],
                     self.transcript().replace("12.34 0.001234", "12.34")):
            self.assertTrue(validate_depth_output(text, 512, 4096, 2048))


class Gem5TickTests(unittest.TestCase):
    def test_last_tick_ignores_untimestamped_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "gem5.log"
            log.write_text("startup\n100: first\nwarning\n250: second\n")
            self.assertEqual(last_gem5_tick(log), 250)

    def test_missing_or_reversed_tick_is_unavailable(self):
        self.assertIsNone(last_gem5_tick(Path("/does/not/exist")))
        self.assertIsNone(tick_delta(None, 10))
        self.assertIsNone(tick_delta(10, 9))
        self.assertEqual(tick_delta(10, 25), 15)


if __name__ == "__main__":
    unittest.main()
