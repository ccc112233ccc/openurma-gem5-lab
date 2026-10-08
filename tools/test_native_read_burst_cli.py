"""Invalid native READ experiments must fail before creating evidence directories."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "integrations/ns3ub/tests/read_burst_contract.py"


class ReadBurstCliTest(unittest.TestCase):
    def test_invalid_count_or_profile_has_no_side_effects(self):
        for args in (("--count", "0"), ("--count", "2049"),
                     ("--count", "512", "--ctp-retransmission", "auto")):
            with self.subTest(args=args), tempfile.TemporaryDirectory() as root:
                output = Path(root) / "not-created"
                result = subprocess.run([sys.executable, str(SCRIPT), "missing-adapter",
                    "missing-peer", str(output), *args], capture_output=True, text=True)
                self.assertEqual(result.returncode, 2)
                self.assertFalse(output.exists())
                self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
