"""Exercise launcher policy blocks without launching guests or network processes."""
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CtpReliabilityPolicyTest(unittest.TestCase):
    def test_launcher_selection_is_independent_of_synchronization(self):
        for name, variable in (("run-qemu-dual.sh", "ctp_retransmission"),
                               ("run-dual.sh", "ctp_retransmission_request")):
            text = (ROOT / "scripts/run" / name).read_text()
            block = re.search(r'case "\$' + variable + r'" in\n.*?\nesac', text, re.S).group()
            for sync in (0, 1):
                for request, expected in (("auto", "off"), ("off", "off"), ("on", "on")):
                    with self.subTest(launcher=name, sync=sync, request=request):
                        code = (f'{variable}={request}\nsync_enabled={sync}\n'
                                f'sync_mode={"required" if sync else "off"}\n'
                                'die() { exit 9; }\n' + block + '\nprintf "%s" "$ctp_retransmission"')
                        result = subprocess.run(["bash", "-c", code], capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(result.stdout, expected)

    def test_adapter_default_and_profile_labels(self):
        text = (ROOT / "integrations/ns3ub/ub-net-adapter.cc").read_text()
        self.assertIn("bool ctp_retransmission{false}", text)
        self.assertIn("experimental-ta-timeout-replay", text)
        self.assertIn("ctp-lossless-lower-layer-assumed", text)

    def test_fault_contracts_keep_explicit_extension_opt_in(self):
        for name in ("read_response_loss_contract.sh", "taack_loss_contract.sh",
                     "retransmission_contract.sh", "retry_exhaustion_contract.sh"):
            text = (ROOT / "integrations/ns3ub/tests" / name).read_text()
            self.assertIn("--ctp-retransmission on", text, name)
        text = (ROOT / "integrations/ns3ub/tests/late_dma_contract.py").read_text()
        self.assertIn('"--ctp-retransmission", "on"', text)


if __name__ == "__main__":
    unittest.main()
