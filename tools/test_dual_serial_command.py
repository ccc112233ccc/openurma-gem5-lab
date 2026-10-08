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
