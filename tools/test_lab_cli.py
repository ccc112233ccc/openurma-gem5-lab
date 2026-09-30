#!/usr/bin/env python3
import contextlib
import io
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from ubsim_lab import cli


class LabCliTest(unittest.TestCase):
    @mock.patch("ubsim_lab.cli.platform.machine", return_value="x86_64")
    @mock.patch("ubsim_lab.cli.platform.system", return_value="Linux")
    def test_auto_runtime_uses_native_on_x86_linux(self, _system, _machine):
        self.assertEqual(cli._runtime("auto"), "native")

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
        self.assertEqual(cli.main(["--runtime=docker", "start", "--print-config"]), 0)
        command = run.call_args.args[0]
        self.assertTrue(command[1].endswith("scripts/run/run-dual.sh"))
        self.assertEqual(command[-1], "--print-config")

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
            cli.main(["--runtime=docker", "start-ready", "--profile", "fast"]),
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

    @mock.patch("ubsim_lab.cli.subprocess.run")
    def test_qemu_dual_always_uses_host_runtime(self, run):
        run.return_value.returncode = 0
        self.assertEqual(cli.main(["--runtime=docker", "start-qemu-dual"]), 0)
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
    def test_x86_setup_requires_native_runtime(self, run):
        error = io.StringIO()
        with contextlib.redirect_stderr(error):
            result = cli.main(["--runtime", "docker", "setup", "--target-arch", "x86_64"])
        self.assertEqual(result, 2)
        self.assertIn("requires '--runtime native'", error.getvalue())
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
