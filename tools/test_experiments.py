"""Exercise recipe limits, retained UART parsing, and simulation summaries."""

from __future__ import annotations

import copy
import unittest
from pathlib import Path
from unittest.mock import patch

from ubsim_lab import experiments, web
from ubsim_lab.cli import ROOT


class ExperimentTests(unittest.TestCase):
    def setUp(self) -> None:
        self.spec = web._read_task("template-topology-gem5-two-heads")
        self.recipe = copy.deepcopy(experiments.DEFAULT_RECIPE)

    def test_recipe_rejects_unsupported_multi_jetty_latency_and_overcommit(self) -> None:
        self.recipe["operations"] = ["read_lat"]
        self.recipe["jettys"] = [1, 2]
        with self.assertRaisesRegex(ValueError, "延迟测试"):
            experiments.validate_recipe(self.recipe, self.spec)
        self.recipe["operations"] = ["read_bw", "write_bw"]
        self.recipe["sizes"] = [512]
        self.recipe["jettys"] = [1, 2, 4]
        self.assertEqual(experiments.validate_recipe(self.recipe, self.spec)["jettys"], [1, 2, 4])
        self.recipe["jettys"] = [47]
        with self.assertRaisesRegex(ValueError, "整除"):
            experiments.validate_recipe(self.recipe, self.spec)

    def test_commands_keep_one_port_and_per_jetty_iterations(self) -> None:
        self.recipe["jettys"] = [1, 2]
        server = experiments.command("write_bw", 512, 2, self.recipe, 21600, False, "gem5")
        client = experiments.command("write_bw", 512, 2, self.recipe, 21600, True, "gem5")
        self.assertIn("-J 2", server)
        self.assertIn("-n 8192", server)
        self.assertIn("-P 21600", server)
        self.assertIn("-S 10.0.0.1", client)
        self.assertEqual(server.count("-P 21600"), 1)

    def test_existing_bandwidth_and_latency_transcripts(self) -> None:
        bandwidth = (ROOT / "experiments/gem5-read-4k-link375-switch500-20261015/read-bw-00-s4096.uart.txt").read_text()
        measured = experiments.parse_output(bandwidth, "read_bw", 4096, 1, 16384)
        self.assertAlmostEqual(measured["average_mib_s"], 46650.62)
        latency = (ROOT / "experiments/gem5-read-lat-link375-switch500-20261015/read-lat-00-s128.uart.txt").read_text()
        measured = experiments.parse_output(latency, "read_lat", 128, 1, 1024)
        self.assertAlmostEqual(measured["average_us"], 3.43)
        with self.assertRaises(ValueError):
            experiments.parse_output(bandwidth.replace("__=0", "__=1", 1), "read_bw", 4096, 1, 16384)

    def test_summary_excludes_warmup_and_keeps_only_simulated_values(self) -> None:
        rows = [{"phase": "warmup", "operation": "read_bw", "size_bytes": 128,
                 "jettys": 1, "average_mib_s": 1, "message_rate_mpps": 1},
                {"phase": "measure", "operation": "read_bw", "size_bytes": 128,
                 "jettys": 1, "average_mib_s": 2800, "message_rate_mpps": 22},
                {"phase": "measure", "operation": "read_bw", "size_bytes": 128,
                 "jettys": 1, "average_mib_s": 2820, "message_rate_mpps": 22.2}]
        result = experiments.summarize(rows)[0]
        self.assertEqual(result["value"], 2810)
        self.assertEqual(result["repeats"], 2)
        self.assertEqual(result["message_rate_mpps"], 22.1)
        self.assertEqual(set(result), {"operation", "size_bytes", "jettys", "repeats",
                                       "metric", "value", "minimum", "maximum",
                                       "message_rate_mpps"})

    def test_checkpoint_availability_explains_missing_multihost_state(self) -> None:
        spec = web._resize_single_leaf(self.spec, 3)
        with patch.object(experiments, "_select_compatible_checkpoint", return_value=None):
            status = experiments.checkpoint_availability(spec)
        self.assertTrue(status["available"])
        self.assertEqual(status["launch_mode"], "cold")
        self.assertIsNone(status["checkpoint"])
        self.assertIn("3 台主机", status["detail"])

    def test_fast_checkpoint_mismatch_names_cpu_model(self) -> None:
        spec = web._resize_single_leaf(self.spec, 3)
        for node in spec["nodes"]:
            node["profile"] = "fast"
        with patch.object(experiments, "_select_compatible_checkpoint", return_value=None):
            status = experiments.checkpoint_availability(spec)
        self.assertTrue(status["available"])
        self.assertEqual(status["launch_mode"], "cold")
        self.assertIn("冷启动", status["detail"])


if __name__ == "__main__":
    unittest.main()
