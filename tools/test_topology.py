#!/usr/bin/env python3
"""Contracts for the topology compiler's launch and rejection behavior."""

import copy
import json
from pathlib import Path
import unittest

from ubsim_lab.config_runtime import resolve_gem5
from ubsim_lab.topology import TopologyError, compile_topology


EXAMPLE = Path(__file__).resolve().parents[1] / "configs/topology-qemu-two-heads.json"
DIRECT = Path(__file__).resolve().parents[1] / "configs/topology-qemu-direct-link.json"


class TopologyTest(unittest.TestCase):
    def setUp(self):
        self.spec = json.loads(EXAMPLE.read_text())

    def test_qemu_plan_carries_topology_into_actual_launcher(self):
        plan = compile_topology(self.spec)
        self.assertEqual(plan["backend"], "native-json")
        self.assertEqual(plan["sync"], "off")
        self.assertEqual(plan["port_count"], 1)
        self.assertEqual(plan["node_links"][0]["latency_ns"], 375)
        self.assertEqual(plan["switch_delay_ns"], 500)
        self.assertEqual(plan["max_jettys"], 47)

    def test_direct_link_plan_has_no_switch_and_splits_propagation(self):
        plan = compile_topology(json.loads(DIRECT.read_text()))
        self.assertIsNone(plan["switch_id"])
        self.assertEqual(plan["fabric_layout"], "direct")
        self.assertEqual(plan["switch_delay_ns"], 0)
        self.assertEqual(plan["node_links"], [
            {"rate_gbps": 400, "latency_ns": 375},
            {"rate_gbps": 400, "latency_ns": 375}])

    def test_direct_link_rejects_switch_and_multiport_shapes(self):
        spec = json.loads(DIRECT.read_text())
        spec["links"][0]["switch"] = "switch-0"
        with self.assertRaisesRegex(TopologyError, "unknown fields"):
            compile_topology(spec)
        spec = json.loads(DIRECT.read_text())
        spec["devices"][0]["ports"] = 2
        spec["devices"][1]["ports"] = 2
        with self.assertRaisesRegex(TopologyError, "direct UB link requires"):
            compile_topology(spec)

    def test_gem5_three_head_plan_uses_checkpoint(self):
        spec = copy.deepcopy(self.spec)
        for node in spec["nodes"]:
            node["simulator"] = "gem5"
            node["profile"] = "modern"
        spec["nodes"].append({"id": "head-c", "simulator": "gem5", "profile": "modern"})
        spec["devices"].append({"id": "udma-c", "node": "head-c", "type": "udma",
                                "eid": 258, "ports": 1, "max_jettys": 47})
        spec["links"].append({"device": "udma-c", "device_port": 0,
                              "switch": "switch-0", "switch_port": 2,
                              "rate_gbps": 400, "latency_ns": 375})
        spec["execution"] = {"sync": "required", "checkpoint": "auto"}
        plan = compile_topology(spec)
        self.assertEqual(len(plan["node_ids"]), 3)
        self.assertEqual(plan["checkpoint"], "auto")
        self.assertEqual(plan["sync"], "required")

    def test_qemu_per_head_cpu_and_memory(self):
        self.spec["nodes"][0].update(cpu="cortex-a72", cores=1, memory_mib=1024)
        self.spec["nodes"][1].update(cpu="neoverse-n1", cores=2, memory_mib=1536)
        plan = compile_topology(self.spec)
        self.assertEqual(plan["machines"][0]["cpu"], "cortex-a72")
        self.assertEqual(plan["machines"][1]["cpu"], "neoverse-n1")
        self.assertEqual(plan["machines"][1]["cores"], 2)
        self.assertEqual(plan["machines"][1]["memory_mib"], 1536)

    def test_qemu_three_heads_have_distinct_devices_and_switch_ports(self):
        third = copy.deepcopy(self.spec["nodes"][0])
        third.update(id="head-c", cpu="neoverse-n1", cores=2, memory_mib=1536)
        self.spec["nodes"].append(third)
        self.spec["devices"].append({"id": "udma-c", "node": "head-c", "type": "udma",
                                     "eid": 258, "ports": 1, "max_jettys": 47})
        self.spec["links"].append({"device": "udma-c", "device_port": 0,
                                   "switch": "switch-0", "switch_port": 2,
                                   "rate_gbps": 400, "latency_ns": 375})
        plan = compile_topology(self.spec)
        self.assertEqual(plan["node_ids"], ["head-a", "head-b", "head-c"])
        self.assertEqual(plan["machines"][2]["memory_mib"], 1536)
        self.assertEqual(len(plan["node_links"]), 3)

    def test_gem5_per_head_override_is_rejected(self):
        for node in self.spec["nodes"]:
            node["simulator"] = "gem5"
            node["profile"] = "modern"
        self.spec["nodes"][1]["cores"] = 2
        with self.assertRaisesRegex(TopologyError, "gem5 per-node"):
            compile_topology(self.spec)

    def test_gem5_machine_tuning_is_explicit_in_launch(self):
        for node in self.spec["nodes"]:
            node.update(simulator="gem5", profile="modern")
        self.spec["machine"] = {"o3_lq_entries": 64,
                                "o3_sq_entries": 64,
                                "l1d_prefetcher": "stride_exclusive"}
        plan = compile_topology(self.spec)
        resolved = resolve_gem5(self.spec, plan)
        self.assertEqual(resolved["o3_lq_entries"], "64")
        self.assertEqual(resolved["o3_sq_entries"], "64")
        self.assertEqual(resolved["cache_prefetcher"], "stride_exclusive")
        self.assertEqual(plan["machines"][0]["o3_lq_entries"], 64)

    def test_gem5_memory_capacity_is_explicit_in_launch(self):
        for node in self.spec["nodes"]:
            node.update(simulator="gem5", profile="modern")
        self.spec["machine"] = {"memory_size_gb": 4}
        plan = compile_topology(self.spec)
        self.assertEqual(resolve_gem5(self.spec, plan)["memory_size"], "4GB")

    def test_fast_topology_resolves_host_latency_and_jetty_reservation(self):
        for node in self.spec["nodes"]:
            node.update(simulator="gem5", profile="fast")
        self.spec["execution"]["checkpoint"] = "auto"
        plan = compile_topology(self.spec)
        resolved = resolve_gem5(self.spec, plan)
        self.assertEqual(resolved["sync_quantum_ns"], "375")
        self.assertEqual(resolved["external_udma_host_latency_ns"], "375")
        self.assertEqual(resolved["max_jettys"], "47")
        self.assertEqual(resolved["public_jettys"], "1")

    def test_qemu_rejects_gem5_machine_tuning(self):
        self.spec["machine"] = {"o3_lq_entries": 64}
        with self.assertRaisesRegex(TopologyError, "only for gem5"):
            compile_topology(self.spec)

    def test_different_node_links_reach_both_backends(self):
        self.spec["links"][1].update(rate_gbps=200, latency_ns=550)
        plan = compile_topology(self.spec)
        self.assertEqual(plan["node_links"], [
            {"rate_gbps": 400, "latency_ns": 375},
            {"rate_gbps": 200, "latency_ns": 550}])
        for node in self.spec["nodes"]:
            node.update(simulator="gem5", profile="modern")
        self.spec["execution"]["checkpoint"] = "auto"
        plan = compile_topology(self.spec)
        resolved = resolve_gem5(self.spec, plan)
        self.assertEqual(resolved["node0_link_rate_gbps"], "400")
        self.assertEqual(resolved["node1_link_rate_gbps"], "200")
        self.assertEqual(resolved["node1_link_latency_ns"], "550")
        self.assertEqual(resolved["sync_quantum_ns"], "100")

    def test_rejects_port_mapping_the_backend_would_ignore(self):
        self.spec["links"][1]["switch_port"] = 2
        with self.assertRaisesRegex(TopologyError, "maps this port"):
            compile_topology(self.spec)

    def test_rejects_mixed_rates_on_one_device(self):
        for device in self.spec["devices"]:
            device["ports"] = 2
        self.spec["links"] = [
            {"device": name, "device_port": port, "switch": "switch-0",
             "switch_port": index * 2 + port, "rate_gbps": 400 if index == 0 and port == 0 else 200,
             "latency_ns": 375}
            for index, name in enumerate(("udma-a", "udma-b")) for port in range(2)
        ]
        with self.assertRaisesRegex(TopologyError, "all ports of one device"):
            compile_topology(self.spec)

    def test_rejects_mixed_simulators(self):
        self.spec["nodes"][1]["simulator"] = "gem5"
        self.spec["nodes"][1]["profile"] = "modern"
        with self.assertRaisesRegex(TopologyError, "mixed gem5/QEMU"):
            compile_topology(self.spec)


if __name__ == "__main__":
    unittest.main()
