"""Focused checks for the local workbench's task and terminal contracts."""

from __future__ import annotations

import copy
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ubsim_lab import web
from ubsim_lab.config_runtime import resolve_gem5
from ubsim_lab.topology import TopologyError, compile_topology


class WorkbenchTaskTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        task_dir = Path(self.temporary.name)
        mocked = patch.object(web, "TASKS", task_dir)
        mocked.start()
        self.addCleanup(mocked.stop)

    def test_create_edit_and_validate_switched_task(self) -> None:
        task = web._create_task({"name": "双机 READ", "simulator": "gem5",
                                 "topology_type": "single_leaf", "host_count": 2})
        self.assertEqual(task["name"], "双机 READ")
        self.assertEqual(len(web._list_tasks()), 3)
        web._atomic_json(web.experiments.recipe_path(task["id"]), web.experiments.DEFAULT_RECIPE)
        self.assertEqual(len(web._list_tasks()), 3)
        spec = web._read_task(task["id"])
        self.assertEqual(compile_topology(spec)["backend"], "native-json")
        spec["links"][0]["latency_ns"] = 410
        spec["switches"][0]["delay_ns"] = 520
        web._save_task(task["id"], {"spec": spec, "display_name": "调整后"})
        saved = web._read_task(task["id"])
        self.assertEqual(saved["links"][0]["latency_ns"], 410)
        self.assertEqual(saved["switches"][0]["delay_ns"], 520)
        self.assertEqual(web._summary(task["id"])["name"], "调整后")
        invalid = copy.deepcopy(saved)
        invalid["links"][0]["switch_port"] = 1
        with self.assertRaises(TopologyError):
            web._save_task(task["id"], {"spec": invalid})
        self.assertEqual(web._read_task(task["id"]), saved)

    def test_generated_star_scale_and_preserved_parameters(self) -> None:
        task = web._create_task({"name": "八机", "simulator": "gem5",
                                 "topology_type": "single_leaf", "host_count": 8})
        spec = web._read_task(task["id"])
        self.assertEqual(len(spec["nodes"]), 8)
        self.assertEqual(len(spec["links"]), 8)
        self.assertEqual(spec["machine"]["memory_size_gb"], 4)
        self.assertEqual(resolve_gem5(spec, compile_topology(spec))["memory_size"], "4GB")
        self.assertEqual([link["switch_port"] for link in spec["links"]], list(range(8)))
        spec["links"][0]["rate_gbps"] = 200
        spec["nodes"][0]["profile"] = "modern"
        resized = web._resize_single_leaf(spec, 3)
        self.assertEqual(resized["links"][0]["rate_gbps"], 200)
        self.assertEqual(len(resized["nodes"]), 3)
        self.assertEqual(len(resized["links"]), 3)
        self.assertEqual(len(web._resize_single_leaf(resized, 8)["links"]), 8)
        self.assertEqual(web._resize_single_leaf(resized, 2)["machine"]["memory_size_gb"], 4)
        compile_topology(resized)
        qemu = web._create_task({"name": "三机", "simulator": "qemu",
                                 "topology_type": "single_leaf", "host_count": 3})
        qemu_spec = web._read_task(qemu["id"])
        self.assertEqual(len(qemu_spec["nodes"]), 3)
        qemu_spec["nodes"][2].update(cpu="neoverse-n1", cores=2, memory_mib=1536)
        self.assertEqual(compile_topology(qemu_spec)["machines"][2]["memory_mib"], 1536)
        self.assertEqual(len(web._resize_single_leaf(qemu_spec, 8)["nodes"]), 8)
        with self.assertRaisesRegex(ValueError, "2–8"):
            web._resize_single_leaf(qemu_spec, 9)
        with self.assertRaisesRegex(ValueError, "只支持单叶"):
            web._create_task({"name": "直连", "simulator": "qemu",
                              "topology_type": "direct", "host_count": 2})

    def test_host_details_follow_effective_machine_profile(self) -> None:
        spec = web._template_spec("gem5", 2)
        modern = web._host_details(spec)
        self.assertEqual(modern[0]["cpu_freq"], "3.2GHz")
        self.assertEqual(modern[0]["cpu_count"], "4")
        self.assertEqual(modern[0]["memory_type"], "DDR5_6400_4x8")
        self.assertEqual(modern[0]["page_size"], "64 KiB")
        for node in spec["nodes"]:
            node["profile"] = "fast"
        fast = web._host_details(spec)
        self.assertEqual(fast[0]["cpu_model"], "AtomicSimpleCPU")
        self.assertEqual(fast[0]["cpu_count"], "1")
        self.assertIsNone(fast[0]["l3"])
        qemu = web._host_details(web._template_spec("qemu", 2))
        self.assertEqual(qemu[0]["cpu_model"], "cortex-a72")
        self.assertEqual(qemu[0]["page_size"], "64 KiB")
        self.assertIsNone(qemu[0]["memory_type"])

    def test_copy_task_keeps_recipe(self) -> None:
        original = web._create_task({"name": "拓扑 A", "simulator": "gem5",
                                     "topology_type": "single_leaf", "host_count": 2})
        spec = web._read_task(original["id"])
        recipe = dict(web.experiments.DEFAULT_RECIPE, sizes=[128], repeats=1)
        web._atomic_json(web.experiments.recipe_path(original["id"]), recipe)
        copied = web._copy_task(original["id"])
        clone = web._read_task(copied["id"])
        self.assertNotEqual(clone["name"], spec["name"])
        self.assertEqual(clone["links"], spec["links"])
        self.assertEqual(web._task_metadata(copied["id"]), {"display_name": "拓扑 A 副本"})
        self.assertEqual(web.experiments.read_recipe(copied["id"], clone), recipe)

    def test_copy_multihost_task_skips_dormant_two_host_recipe(self) -> None:
        original = web._create_task({"name": "先双机后多机", "simulator": "gem5",
                                     "topology_type": "single_leaf", "host_count": 2})
        recipe = dict(web.experiments.DEFAULT_RECIPE, sizes=[128], repeats=1)
        web._atomic_json(web.experiments.recipe_path(original["id"]), recipe)
        expanded = web._resize_single_leaf(web._read_task(original["id"]), 4)
        web._save_task(original["id"], {"spec": expanded})
        copied = web._copy_task(original["id"])
        self.assertEqual(len(web._read_task(copied["id"])["nodes"]), 4)
        self.assertFalse(web.experiments.recipe_path(copied["id"]).exists())

    def test_task_ids_cannot_escape_task_directory(self) -> None:
        with self.assertRaises(ValueError):
            web._task_path("../../README")
        with self.assertRaises(ValueError):
            web._task_path("template-random")

    def test_runtime_reports_memory_guard_stop(self) -> None:
        run_dir = Path(self.temporary.name) / "run"
        run_dir.mkdir()
        (run_dir / "topology.json").write_text('{"name":"sample"}')
        (run_dir / "memory-guard.stopped").write_text("available memory below reserve\n")
        with patch.object(web, "_run_dir", return_value=run_dir):
            runtime = web._runtime("gem5")
        self.assertFalse(runtime["active"])
        self.assertEqual(runtime["stop_reason"], "available memory below reserve")

    def test_template_topology_is_read_only_but_recipe_can_be_saved(self) -> None:
        task_id = "template-topology-gem5-two-heads"
        path = web.experiments.recipe_path(task_id)
        recipe = dict(web.experiments.DEFAULT_RECIPE, sizes=[128], repeats=1)
        web._atomic_json(path, recipe)
        self.assertEqual(web.experiments.read_recipe(task_id, web._read_task(task_id))["sizes"], [128])
        with self.assertRaises(ValueError):
            web.experiments.recipe_path("../../unsafe")


class WebSocketFrameTests(unittest.TestCase):
    def test_masked_input_and_output_frame(self) -> None:
        mask = b"abcd"
        payload = b"uname\r"
        wire = bytearray(b"\x81" + bytes([0x80 | len(payload)]) + mask +
                         bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload)))
        self.assertEqual(web._ws_parse(wire), [(1, payload)])
        self.assertEqual(wire, bytearray())
        self.assertEqual(web._ws_frame(b"ok"), b"\x82\x02ok")


if __name__ == "__main__":
    unittest.main()
