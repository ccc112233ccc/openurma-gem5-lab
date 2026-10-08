"""Observer parsing tests; these do not validate the hardware model itself."""
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from run_qemu_timing_trial import observe_drain


class DrainObservationTest(unittest.TestCase):
    def run_sample(self, active=False, fresh=True, truncated=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            names = ("udma-node0/udma", "udma-node1/udma", "ub-fabric/ns3")
            lines = ["[UDMA_PROGRESS] quiescent=1\n"] * 2 + [
                "[NS3_UB_NET_PROGRESS] native_tasks=0 pending_target_dma=0 "
                "scheduled_packets=0 outputs_empty=1\n"]
            for pid, name in enumerate(names, 10):
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / f"{name}.pid").write_text(str(pid))
                # Old idle snapshots must never satisfy a new signal request.
                (root / f"{name}.log").write_text(lines[pid - 10])

            def signal_reply(pid, unused_signal):
                if not fresh:
                    return
                line = lines[pid - 10]
                if active and pid == 12:
                    line = line.replace("pending_target_dma=0", "pending_target_dma=1")
                if truncated:
                    line = line.rstrip("\n")
                with (root / f"{names[pid - 10]}.log").open("a") as stream:
                    stream.write(line)

            with patch("run_qemu_timing_trial.os.kill", side_effect=signal_reply):
                return observe_drain(root, 0.35)

    def test_two_fresh_empty_samples(self):
        result = self.run_sample()
        self.assertTrue(result["observed_work_drained"])
        self.assertGreaterEqual(len(result["samples"]), 2)

    def test_pending_fabric_work_rejected(self):
        self.assertFalse(self.run_sample(active=True)["observed_work_drained"])

    def test_stale_empty_samples_rejected(self):
        result = self.run_sample(fresh=False)
        self.assertFalse(result["observed_work_drained"])
        self.assertEqual(result["samples"][0]["processes"], {})

    def test_incomplete_snapshot_rejected(self):
        result = self.run_sample(truncated=True)
        self.assertFalse(result["observed_work_drained"])
        self.assertEqual(result["samples"][0]["processes"], {})


if __name__ == "__main__":
    unittest.main()
