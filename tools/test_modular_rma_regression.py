import tempfile
import unittest
from pathlib import Path

from tools.run_modular_rma_regression import last_gem5_tick, tick_delta


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
