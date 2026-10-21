#!/usr/bin/env python3
import contextlib
import hashlib
import io
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from ubsim_lab import cli


class LabCliTest(unittest.TestCase):
    def test_node_template_matches_two_or_three_hosts_with_same_machine(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            image = root / "out/official-udma-64k-20261015.cpio.gz"
            image.parent.mkdir()
            image.write_bytes(b"template guest")
            directory = root / "checkpoints/template"
            (directory / "template-cpt").mkdir(parents=True)
            (directory / "template-cpt/m5.cpt").write_text("state")
            (directory / "run-manifest.txt").write_text(
                "node_count=2\nprofile=modern\nmemory_size=4GB\n"
                "template_checkpoint=1\nnode0_link_latency_ns=375\n"
                "node1_link_latency_ns=375\n")
            (directory / "checkpoint-manifest.txt").write_text(
                "checkpoint_kind=node-template-v1\n"
                f"initramfs_sha256={hashlib.sha256(image.read_bytes()).hexdigest()}\n")
            desired = {"node_count": "3", "profile": "modern",
                       "memory_size": "4GB", "template_checkpoint": "0",
                       "node0_link_latency_ns": "375",
                       "node1_link_latency_ns": "375",
                       "node2_link_latency_ns": "375"}
            with mock.patch.object(cli, "ROOT", root), mock.patch.dict(
                os.environ, {}, clear=True
            ):
                self.assertEqual(cli._select_compatible_checkpoint(desired),
                                 "template")
                desired["node_count"] = "2"
                self.assertEqual(cli._select_compatible_checkpoint(desired),
                                 "template")
                desired["memory_size"] = "32GB"
                self.assertIsNone(cli._select_compatible_checkpoint(desired))

    def test_checkpoint_selection_rejects_stale_guest_image(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            image = root / "out/official-udma-64k-20261015.cpio.gz"
            image.parent.mkdir()
            image.write_bytes(b"new guest")
            current_hash = hashlib.sha256(image.read_bytes()).hexdigest()
            for name, image_hash, created in (
                ("old", "stale", "2026-10-10T01:00:00Z"),
                ("current", current_hash, "2026-10-09T01:00:00Z"),
            ):
                directory = root / "checkpoints" / name
                directory.mkdir(parents=True)
                (directory / "run-manifest.txt").write_text(
                    "node_count=2\nprofile=modern\n")
                (directory / "checkpoint-manifest.txt").write_text(
                    f"created_utc={created}\ninitramfs_sha256={image_hash}\n")
                for node in range(2):
                    (directory / f"node{node}-cpt").mkdir()
                    (directory / f"udma-node{node}.state").touch()
            with mock.patch.object(cli, "ROOT", root), mock.patch.dict(
                os.environ, {}, clear=True
            ):
                self.assertEqual(cli._select_compatible_checkpoint(
                    {"node_count": "2", "profile": "modern"}), "current")
                image.unlink()
                self.assertIsNone(cli._select_compatible_checkpoint(
                    {"node_count": "2", "profile": "modern"}))

    def test_auto_runtime_uses_native(self):
        self.assertEqual(cli._runtime("auto"), "native")

    def test_docker_runtime_is_rejected(self):
        error = io.StringIO()
        with contextlib.redirect_stderr(error):
            self.assertEqual(cli.main(["--runtime=docker", "start"]), 2)
        self.assertIn("only native runtime is supported", error.getvalue())

    def test_help_lists_lifecycle(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(cli.main(["help"]), 0)
        self.assertIn("attach NODE", output.getvalue())
        self.assertIn("build TARGET", output.getvalue())

    @mock.patch("ubsim_lab.cli.subprocess.run")
    def test_attach_maps_node_to_uart(self, run):
        run.return_value.returncode = 0
        with mock.patch.dict(os.environ, {"UBSIM_DUAL_UART0": "3460", "UBSIM_DUAL_UART1": "3470"}, clear=False):
            self.assertEqual(cli.main(["--runtime", "native", "attach", "3"]), 0)
        env = run.call_args.kwargs["env"]
        self.assertEqual(env["UBSIM_M5TERM_PORT"], "3490")
        self.assertEqual(env["UBSIM_EXECUTION_MODE"], "native")

    @mock.patch("ubsim_lab.cli.subprocess.run")
    def test_start_selects_internal_backend(self, run):
        run.return_value.returncode = 0
        self.assertEqual(cli.main(["start", "--print-config"]), 0)
        command = run.call_args.args[0]
        self.assertTrue(command[1].endswith("scripts/run/run-dual.sh"))
        self.assertEqual(command[-1], "--print-config")

    @mock.patch("ubsim_lab.cli.subprocess.run")
    def test_native_keeps_explicit_lab_root(self, run):
        run.return_value.returncode = 0
        with mock.patch.dict(os.environ, {"UBSIM_LAB_ROOT": "/workspace/ubsim-gem5-lab"}):
            self.assertEqual(cli.main(["--runtime=native", "start", "--print-config"]), 0)
        self.assertEqual(run.call_args.kwargs["env"]["UBSIM_LAB_ROOT"],
                         "/workspace/ubsim-gem5-lab")

    @mock.patch("ubsim_lab.cli.subprocess.run")
    def test_umdk_build_keeps_official_provider_option(self, run):
        run.return_value.returncode = 0
        with mock.patch.dict(os.environ, {"BUILD_STOCK_UDMA": "enable"}):
            self.assertEqual(cli.main(["build", "umdk"]), 0)
        self.assertEqual(run.call_args.kwargs["env"]["BUILD_STOCK_UDMA"], "enable")
        self.assertIn("scripts/build/build_umdk.sh", run.call_args.args[0][1])

    @mock.patch("ubsim_lab.cli._select_compatible_checkpoint",
                return_value="shell-ready-current")
    @mock.patch("ubsim_lab.cli.subprocess.run")
    def test_start_ready_probes_and_restores_compatible_checkpoint(
        self, run, _select
    ):
        run.side_effect = [
            SimpleNamespace(returncode=0, stdout="node_count=2\nprofile=fast\n",
                            stderr=""),
            SimpleNamespace(returncode=0),
        ]
        self.assertEqual(
            cli.main(["start-ready", "--profile", "fast"]),
            0,
        )
        probe = run.call_args_list[0].args[0]
        launch = run.call_args_list[1].args[0]
        self.assertEqual(probe[-1], "--print-config")
        self.assertEqual(launch[-2:], ["--restore-checkpoint", "shell-ready-current"])

    @mock.patch("ubsim_lab.cli.subprocess.run")
    def test_start_ready_rejects_manual_restore(self, run):
        error = io.StringIO()
        with contextlib.redirect_stderr(error):
            result = cli.main(["start-ready", "--restore-checkpoint", "old"])
        self.assertEqual(result, 2)
        self.assertIn("selects the checkpoint automatically", error.getvalue())
        run.assert_not_called()

    @mock.patch("ubsim_lab.cli.subprocess.run")
    def test_start_ready_help_is_forwarded_without_checkpoint_lookup(self, run):
        run.return_value.returncode = 0
        self.assertEqual(cli.main(["start-ready", "--help"]), 0)
        self.assertEqual(run.call_args.args[0][-1], "--help")

    def test_checkpoint_selection_rejects_old_udma_state_format(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / "checkpoints" / "ready"
            directory.mkdir(parents=True)
            for node in range(2):
                (directory / f"node{node}-cpt").mkdir()
                (directory / f"udma-node{node}.state").touch()
            desired = {"node_count": "2", "udma_state_format": "3"}
            with mock.patch.object(cli, "ROOT", root):
                for version in (None, "2", "3"):
                    manifest = "node_count=2\n"
                    if version is not None:
                        manifest += f"udma_state_format={version}\n"
                    (directory / "run-manifest.txt").write_text(manifest)
                    self.assertEqual(cli._select_compatible_checkpoint(desired),
                                     "ready" if version == "3" else None)
                    self.assertEqual(cli._select_compatible_checkpoint(
                        {"node_count": "2", "udma_state_format": "4"}),
                        "ready" if version == "3" else None)
                (directory / "run-manifest.txt").write_text(
                    "node_count=2\nudma_state_format=4\n")
                self.assertEqual(cli._select_compatible_checkpoint(
                    {"node_count": "2", "udma_state_format": "4"}), "ready")
                self.assertIsNone(cli._select_compatible_checkpoint(desired))

    def test_checkpoint_selection_defaults_old_auto_o3_policy_to_off(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / "checkpoints" / "ready"
            directory.mkdir(parents=True)
            (directory / "run-manifest.txt").write_text("node_count=2\n")
            for node in range(2):
                (directory / f"node{node}-cpt").mkdir()
                (directory / f"udma-node{node}.state").touch()
            with mock.patch.object(cli, "ROOT", root):
                self.assertEqual(cli._select_compatible_checkpoint(
                    {"node_count": "2", "auto_o3_lifecycle": "0"}), "ready")
                self.assertIsNone(cli._select_compatible_checkpoint(
                    {"node_count": "2", "auto_o3_lifecycle": "1"}))
                self.assertEqual(cli._select_compatible_checkpoint(
                    {"node_count": "2", "fast_boot_noncaching": "0"}), "ready")
                self.assertIsNone(cli._select_compatible_checkpoint(
                    {"node_count": "2", "fast_boot_noncaching": "1"}))
                self.assertEqual(cli._select_compatible_checkpoint(
                    {"node_count": "2", "max_jfs_depth": "1024",
                     "max_jfr_depth": "1024", "max_jfc_depth": "4096"}), "ready")
                self.assertIsNone(cli._select_compatible_checkpoint(
                    {"node_count": "2", "max_jfs_depth": "8192"}))
                self.assertEqual(cli._select_compatible_checkpoint(
                    {"node_count": "2", "max_read_outstanding": "64"}),
                    "ready")

    def test_checkpoint_selection_requires_complete_matching_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            checkpoint_root = root / "checkpoints"

            def create(name, created, profile="fast", complete=True):
                directory = checkpoint_root / name
                directory.mkdir(parents=True)
                (directory / "run-manifest.txt").write_text(
                    f"node_count=2\nprofile={profile}\nrestore_checkpoint=none\n"
                )
                (directory / "checkpoint-manifest.txt").write_text(
                    f"created_utc={created}\n"
                )
                for node in range(2):
                    (directory / f"node{node}-cpt").mkdir()
                    if complete or node == 0:
                        (directory / f"udma-node{node}.state").touch()

            create("older-compatible", "2026-09-29T01:00:00Z")
            create("newer-compatible", "2026-09-30T01:00:00Z")
            create("newest-incomplete", "2026-10-01T01:00:00Z", complete=False)
            create("newest-wrong-profile", "2026-10-02T01:00:00Z", profile="server")

            desired = {
                "node_count": "2",
                "profile": "fast",
                "restore_checkpoint": "none",
            }
            with mock.patch.object(cli, "ROOT", root):
                self.assertEqual(
                    cli._select_compatible_checkpoint(desired),
                    "newer-compatible",
                )

    def test_old_uniform_checkpoint_matches_only_uniform_endpoint_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / "checkpoints" / "uniform"
            directory.mkdir(parents=True)
            (directory / "run-manifest.txt").write_text(
                "node_count=2\npeer_latency_ns=375\npeer_link_rate_gbps=400\n")
            for node in range(2):
                (directory / f"node{node}-cpt").mkdir()
                (directory / f"udma-node{node}.state").touch()
            desired = {"node_count": "2", "peer_latency_ns": "375",
                       "peer_link_rate_gbps": "400",
                       "node0_link_latency_ns": "375", "node1_link_latency_ns": "375",
                       "node0_link_rate_gbps": "400", "node1_link_rate_gbps": "400"}
            with mock.patch.object(cli, "ROOT", root):
                self.assertEqual(cli._select_compatible_checkpoint(desired), "uniform")
                desired["node1_link_rate_gbps"] = "200"
                self.assertIsNone(cli._select_compatible_checkpoint(desired))

    def test_quiescent_checkpoint_can_change_fabric_without_changing_device(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / "checkpoints" / "switched"
            directory.mkdir(parents=True)
            (directory / "run-manifest.txt").write_text(
                "node_count=2\nub_port_count=1\npeer_switch_delay=500ns\n"
                "peer_latency_ns=375\npeer_link_rate_gbps=400\n")
            for node in range(2):
                (directory / f"node{node}-cpt").mkdir()
                (directory / f"udma-node{node}.state").touch()
            desired = {"node_count": "2", "ub_port_count": "1",
                       "peer_switch_delay": "0ns", "ub_fabric_layout": "direct",
                       "peer_latency_ns": "375", "peer_link_rate_gbps": "400"}
            with mock.patch.object(cli, "ROOT", root):
                self.assertEqual(cli._select_compatible_checkpoint(desired), "switched")
                desired["ub_port_count"] = "2"
                self.assertIsNone(cli._select_compatible_checkpoint(desired))

    @mock.patch("ubsim_lab.cli.subprocess.run")
    def test_qemu_dual_always_uses_host_runtime(self, run):
        run.return_value.returncode = 0
        self.assertEqual(cli.main(["start-qemu-dual"]), 0)
        self.assertEqual(
            run.call_args.kwargs["env"]["UBSIM_EXECUTION_MODE"], "native"
        )
        self.assertTrue(
            run.call_args.args[0][1].endswith("scripts/run/run-qemu-dual.sh")
        )

    @mock.patch("ubsim_lab.cli.subprocess.run")
    def test_x86_userspace_target_is_routed(self, run):
        run.return_value.returncode = 0
        self.assertEqual(
            cli.main(["--runtime", "native", "build", "umdk", "--target-arch", "x86_64"]),
            0,
        )
        self.assertIn("scripts/build/build_umdk.sh", run.call_args.args[0][1])

    @mock.patch("ubsim_lab.cli.subprocess.run")
    def test_x86_full_system_target_is_rejected(self, run):
        error = io.StringIO()
        with contextlib.redirect_stderr(error):
            result = cli.main(["build", "kernel", "--target-arch", "x86_64"])
        self.assertEqual(result, 2)
        self.assertIn("userspace target only", error.getvalue())
        run.assert_not_called()

    @mock.patch("ubsim_lab.cli.subprocess.run")
    def test_x86_setup_uses_native_script(self, run):
        run.return_value.returncode = 0
        self.assertEqual(cli.main(["setup", "--target-arch", "x86_64"]), 0)
        self.assertIn("scripts/setup-native.sh", run.call_args.args[0][1])


if __name__ == "__main__":
    unittest.main()
