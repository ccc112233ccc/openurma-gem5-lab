#!/usr/bin/env python3
import contextlib
import io
import os
import unittest
from unittest import mock

from openurma_lab import cli


class LabCliTest(unittest.TestCase):
    @mock.patch("openurma_lab.cli.platform.machine", return_value="x86_64")
    @mock.patch("openurma_lab.cli.platform.system", return_value="Linux")
    def test_auto_runtime_uses_native_on_x86_linux(self, _system, _machine):
        self.assertEqual(cli._runtime("auto"), "native")

    def test_help_lists_lifecycle(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(cli.main(["help"]), 0)
        self.assertIn("attach NODE", output.getvalue())
        self.assertIn("build TARGET", output.getvalue())

    @mock.patch("openurma_lab.cli.subprocess.run")
    def test_attach_maps_node_to_uart(self, run):
        run.return_value.returncode = 0
        with mock.patch.dict(os.environ, {"OPENURMA_DUAL_UART0": "3460", "OPENURMA_DUAL_UART1": "3470"}, clear=False):
            self.assertEqual(cli.main(["--runtime", "native", "attach", "3"]), 0)
        env = run.call_args.kwargs["env"]
        self.assertEqual(env["OPENURMA_M5TERM_PORT"], "3490")
        self.assertEqual(env["OPENURMA_EXECUTION_MODE"], "native")

    @mock.patch("openurma_lab.cli.subprocess.run")
    def test_start_selects_internal_backend(self, run):
        run.return_value.returncode = 0
        self.assertEqual(cli.main(["--runtime=docker", "start", "--print-config"]), 0)
        command = run.call_args.args[0]
        self.assertTrue(command[1].endswith("scripts/run/run-dual.sh"))
        self.assertEqual(command[-1], "--print-config")

    @mock.patch("openurma_lab.cli.subprocess.run")
    def test_x86_userspace_target_is_routed(self, run):
        run.return_value.returncode = 0
        self.assertEqual(
            cli.main(["--runtime", "native", "build", "umdk", "--target-arch", "x86_64"]),
            0,
        )
        self.assertIn("scripts/build/build_umdk.sh", run.call_args.args[0][1])

    @mock.patch("openurma_lab.cli.subprocess.run")
    def test_x86_full_system_target_is_rejected(self, run):
        error = io.StringIO()
        with contextlib.redirect_stderr(error):
            result = cli.main(["build", "kernel", "--target-arch", "x86_64"])
        self.assertEqual(result, 2)
        self.assertIn("userspace target only", error.getvalue())
        run.assert_not_called()

    @mock.patch("openurma_lab.cli.subprocess.run")
    def test_x86_setup_requires_native_runtime(self, run):
        error = io.StringIO()
        with contextlib.redirect_stderr(error):
            result = cli.main(["--runtime", "docker", "setup", "--target-arch", "x86_64"])
        self.assertEqual(result, 2)
        self.assertIn("requires '--runtime native'", error.getvalue())
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
