"""Safety checks for the host-memory guard without starting gem5."""

from __future__ import annotations

import tempfile
from pathlib import Path
from unittest import TestCase, mock

from tools import host_memory_guard as guard


class MemoryGuardTest(TestCase):
    def test_checkpoint_rejects_three_large_guests_on_small_host(self) -> None:
        with mock.patch.object(guard, "memory_bytes", return_value=(16 * guard.GIB,
                                                                   12 * guard.GIB)):
            self.assertEqual(2, guard.check("checkpoint", 3, "32GB"))
            self.assertEqual(0, guard.check("checkpoint", 2, "32GB"))
            self.assertEqual(2, guard.check("start", 8, "4GB"))
            self.assertEqual(2, guard.check("qemu-start", 2, "8192MiB"))
            self.assertEqual(0, guard.check("qemu-start", 2, "1024MiB"))

    def test_monitor_stops_only_after_sustained_low_memory(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            run_root = Path(root)
            (run_root / "run-manifest.txt").write_text("node_count=2\n")
            samples = [(16 * guard.GIB, 8 * guard.GIB),
                       (16 * guard.GIB, 3 * guard.GIB),
                       (16 * guard.GIB, 2 * guard.GIB)]
            with mock.patch.object(guard, "memory_bytes", side_effect=samples), \
                 mock.patch.object(guard, "live_guest", return_value=True), \
                 mock.patch.object(guard.time, "sleep"), \
                 mock.patch.object(guard.subprocess, "run") as stop:
                result = guard.monitor(run_root, 2, Path("/tmp/stop.sh"), 0)
            self.assertEqual(2, result)
            stop.assert_called_once()
            self.assertIn("fell below reserve", (run_root / "memory-guard.stopped").read_text())
