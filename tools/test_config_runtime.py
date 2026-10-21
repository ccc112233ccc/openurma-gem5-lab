"""Parity and direct-launch contracts for JSON-driven simulation tasks."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from ubsim_lab import experiments
from ubsim_lab import topology
from ubsim_lab.config_launcher import gem5_command
from ubsim_lab.config_runtime import resolve_gem5
from ubsim_lab.topology import compile_topology


ROOT = Path(__file__).resolve().parents[1]


class ConfigRuntimeTests(unittest.TestCase):
    def test_gem5_json_manifest_matches_previous_launcher_contract(self):
        for filename in ("topology-gem5-two-heads.json",
                         "topology-gem5-asymmetric-links.json",
                         "topology-gem5-direct-link.json"):
            with self.subTest(filename=filename):
                spec = json.loads((ROOT / "configs" / filename).read_text())
                plan = compile_topology(spec)
                env = os.environ.copy()
                env.update({
                    "UBSIM_NODE_LINK_LATENCIES_NS": ",".join(
                        str(link["latency_ns"]) for link in plan["node_links"]),
                    "UBSIM_NODE_LINK_RATES_GBPS": ",".join(
                        str(link["rate_gbps"]) for link in plan["node_links"]),
                })
                if plan["fabric_layout"] == "direct":
                    env["UBSIM_UB_FABRIC_LAYOUT"] = "direct"
                profile = spec["nodes"][0]["profile"]
                latency = min(link["latency_ns"] for link in plan["node_links"])
                flags = ["--nodes", str(len(plan["node_ids"])), "--profile", profile,
                         "--provider", "official", "--ub-port-count", str(plan["port_count"]),
                         "--peer-link-rate-gbps", str(plan["node_links"][0]["rate_gbps"]),
                         "--peer-latency-ns", str(latency),
                         "--sync-quantum-ns", str(min(100, latency) if profile == "modern" else latency),
                         "--peer-switch-delay", f"{plan['switch_delay_ns']}ns",
                         "--roi-sync" if plan["sync"] == "required" else "--no-sync"]
                if plan["jetty_limit_explicit"]:
                    flags.extend(("--max-jettys", str(plan["max_jettys"]),
                                  "--public-jettys", "1"))
                for field, flag in (("o3_lq_entries", "--o3-lq-entries"),
                                    ("o3_sq_entries", "--o3-sq-entries"),
                                    ("l1d_prefetcher", "--l1d-prefetcher"),
                                    ("memory_size_gb", "--mem-size")):
                    if field in spec.get("machine", {}):
                        value = spec["machine"][field]
                        flags.extend((flag, f"{value}GB" if field == "memory_size_gb" else str(value)))
                result = subprocess.run(
                    ["bash", str(ROOT / "scripts/run/run-dual.sh"),
                     *flags, "--print-config"],
                    cwd=ROOT, env=env, capture_output=True, text=True, check=True)
                previous = dict(line.split("=", 1) for line in result.stdout.splitlines()
                                if "=" in line)
                self.assertEqual(resolve_gem5(spec, plan), previous)

    def test_checkpoint_preflight_does_not_execute_shell_launcher(self):
        spec = json.loads((ROOT / "configs/topology-gem5-two-heads.json").read_text())
        with patch.object(experiments.subprocess, "run", side_effect=AssertionError(
                "checkpoint preflight invoked a shell process")):
            status = experiments.checkpoint_availability(spec)
        self.assertIn("profile", status["resolved"])
        self.assertIn("cpu_mode", status["resolved"])

    def test_gem5_cold_start_command_has_no_restore_source(self):
        spec = json.loads((ROOT / "configs/topology-gem5-two-heads.json").read_text())
        spec["execution"]["checkpoint"] = "none"
        resolved = resolve_gem5(spec, compile_topology(spec))
        with tempfile.TemporaryDirectory() as directory:
            command = gem5_command(resolved, 0, Path(directory), None, False)
        self.assertFalse(any(item.startswith("--restore-from=") for item in command))
        self.assertIn("--script=", command)

    def test_fast_auto_without_checkpoint_reaches_cold_start_guard(self):
        from ubsim_lab import config_launcher
        spec = json.loads((ROOT / "configs/topology-gem5-two-heads.json").read_text())
        for node in spec["nodes"]:
            node["profile"] = "fast"
        plan = compile_topology(spec)
        with patch.object(config_launcher, "_select_compatible_checkpoint", return_value=None), \
             patch.object(config_launcher, "_check_memory", side_effect=RuntimeError("cold-start-guard")):
            with self.assertRaisesRegex(RuntimeError, "cold-start-guard"):
                config_launcher.start_gem5(spec, plan)

    def test_implicit_fast_jetty_defaults_remain_compatible(self):
        spec = json.loads((ROOT / "configs/topology-gem5-two-heads.json").read_text())
        for node in spec["nodes"]:
            node["profile"] = "fast"
        for device in spec["devices"]:
            device.pop("max_jettys")
        resolved = resolve_gem5(spec, compile_topology(spec))
        self.assertEqual((resolved["max_jettys"], resolved["public_jettys"]),
                         ("1152", "128"))

    def test_modern_no_sync_uses_atomic_boot_contract(self):
        spec = json.loads((ROOT / "configs/topology-gem5-two-heads.json").read_text())
        spec["execution"]["sync"] = "off"
        resolved = resolve_gem5(spec, compile_topology(spec))
        self.assertEqual(resolved["initial_memory_mode"], "atomic")
        self.assertEqual(resolved["cpu_switch_policy"],
                         "send_lat_roi_enter_o3_exit_atomic")
        self.assertEqual(resolved["lifecycle_sync"], "0")

    def test_topology_start_calls_native_launcher(self):
        spec = json.loads((ROOT / "configs/topology-gem5-two-heads.json").read_text())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "task.json"
            path.write_text(json.dumps(spec))
            with patch("ubsim_lab.config_launcher.start_gem5", return_value=Path(directory)) as start:
                self.assertEqual(topology.run(["start", str(path)], "native", ROOT), 0)
            start.assert_called_once()
            saved = json.loads((Path(directory) / "topology-plan.json").read_text())
            self.assertEqual(saved["launch"], ["./lab", "topology", "start", str(path)])
            self.assertNotIn("command", saved)


if __name__ == "__main__":
    unittest.main()
