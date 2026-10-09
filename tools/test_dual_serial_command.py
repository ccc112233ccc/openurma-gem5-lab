#!/usr/bin/env python3

from __future__ import annotations

import unittest
from unittest import mock

from dual_serial_command import SHELL_PROMPT_PATTERN, connect_until_ready, run_one


class ConsoleEofTests(unittest.TestCase):
    def test_eof_is_not_reported_as_timeout(self) -> None:
        for replies, phase in (([b""], "before command"),
                               ([b"(ubsim-node0) ~ # ", b""], "awaiting command completion")):
            with self.subTest(phase=phase):
                sock = mock.MagicMock()
                sock.__enter__.return_value = sock
                sock.recv.side_effect = replies
                results = {}
                with mock.patch("dual_serial_command.connect_until_ready", return_value=sock):
                    run_one(3560, "true", 180, 0, None, mock.Mock(), results)
                self.assertIn(phase, results[3560])
                self.assertIn("(EOF)", results[3560])
                self.assertNotIn("within 180s", results[3560])


class CommandBoundaryTests(unittest.TestCase):
    def run_replies(self, replies: list[bytes]) -> tuple[str, bytes]:
        sock = mock.MagicMock()
        sock.__enter__.return_value = sock
        sock.recv.side_effect = [b"(ubsim-node0) ~ # ", *replies]
        results = {}
        with mock.patch("dual_serial_command.connect_until_ready", return_value=sock), \
                mock.patch("dual_serial_command.time.monotonic_ns", return_value=123):
            run_one(3560, "true", 180, 0, None, mock.Mock(), results)
        return results[3560], sock.sendall.call_args.args[0]

    def test_delayed_history_and_echo_are_excluded(self) -> None:
        output, sent = self.run_replies([
            b"4096 64 999 999 999\r\n(ubsim-node0) ~ # ",
            b"printf '\\n__UBSIM_BEGIN_3560_123__\\n'; true; old echo\r\n",
            b"old scan row\r\n\r\n__UBSIM_BEGIN_3560_123__\r\n",
            b"current result\r\n__UBSIM_RC_3560_123__=0\r\n(ubsim-node0) ~ # ",
        ])
        self.assertTrue(output.startswith("current result"))
        self.assertNotIn("old", output)
        self.assertNotIn("999", output)
        self.assertNotIn("BEGIN", output)
        self.assertIn(b"printf '\\n__UBSIM_BEGIN_3560_123__\\n'; true", sent)

    def test_split_begin_status_and_prompt(self) -> None:
        output, _ = self.run_replies([
            b"history\r\n__UBSIM_BE", b"GIN_3560_123__\r", b"\nnew\r\n__UBSIM_R",
            b"C_3560_123__=0\r", b"\n(ubsim-", b"node0) ~ # ",
        ])
        self.assertTrue(output.startswith("new\r\n"))
        self.assertNotIn("history", output)

    def test_echoed_begin_and_status_without_executed_begin_are_rejected(self) -> None:
        output, _ = self.run_replies([
            b"printf '\\n__UBSIM_BEGIN_3560_123__\\n'; true\r\n",
            b"__UBSIM_RC_3560_123__=0\r\n(ubsim-node0) ~ # ", b"",
        ])
        self.assertTrue(output.startswith("ERROR:"))
        self.assertIn("(EOF)", output)

    def test_nonzero_status_after_begin_remains_failure(self) -> None:
        output, _ = self.run_replies([
            b"\n__UBSIM_BEGIN_3560_123__\nfailed\n",
            b"__UBSIM_RC_3560_123__=22\n(ubsim-node0) ~ # ",
        ])
        self.assertTrue(output.startswith("ERROR: command exited with status 22"))
        self.assertIn("failed", output)


class PromptPatternTests(unittest.TestCase):
    def test_accepts_current_and_checkpoint_hostnames(self) -> None:
        import re

        for prompt in (b"(ubsim-node0) ~ # ", b"(openurma-node1) ~ # "):
            with self.subTest(prompt=prompt):
                self.assertIsNotNone(re.search(SHELL_PROMPT_PATTERN, prompt))

    def test_rejects_boot_output(self) -> None:
        import re

        self.assertIsNone(re.search(SHELL_PROMPT_PATTERN, b"[  1.23] booting Linux"))


class ConnectUntilReadyTests(unittest.TestCase):
    def test_retries_until_delayed_listener_appears(self) -> None:
        connected = object()
        with mock.patch(
            "dual_serial_command.socket.create_connection",
            side_effect=[ConnectionRefusedError(), connected],
        ) as create_connection, mock.patch(
            "dual_serial_command.time.monotonic", return_value=0.0
        ), mock.patch("dual_serial_command.time.sleep") as sleep:
            self.assertIs(connect_until_ready(3460, 1.0), connected)
        self.assertEqual(create_connection.call_count, 2)
        sleep.assert_called_once()

    def test_timeout_reports_uart_port(self) -> None:
        with mock.patch(
            "dual_serial_command.socket.create_connection",
            side_effect=ConnectionRefusedError("not ready"),
        ), mock.patch(
            "dual_serial_command.time.monotonic", side_effect=[0.0, 0.0, 0.0, 2.0]
        ), mock.patch("dual_serial_command.time.sleep"):
            with self.assertRaisesRegex(TimeoutError, r"UART 3460"):
                connect_until_ready(3460, 1.0)


if __name__ == "__main__":
    unittest.main()
