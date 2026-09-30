#!/usr/bin/env python3
"""Stop all guests at the same gem5 checkpoint pseudo operation."""

from __future__ import annotations

import argparse
import re
import socket
import threading
import time


SHELL_PROMPT_PATTERN = rb"\([A-Za-z0-9][A-Za-z0-9_.-]*\)[^\r\n]*# "


def checkpoint_one(port: int, timeout: float, gate: threading.Barrier,
                   errors: list[str]) -> None:
    deadline = time.monotonic() + timeout
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=5) as stream:
            stream.settimeout(0.2)
            data = b""
            kicked = False
            while time.monotonic() < deadline:
                try:
                    chunk = stream.recv(65536)
                except socket.timeout:
                    if not kicked:
                        stream.sendall(b"\r")
                        kicked = True
                    continue
                if not chunk:
                    raise RuntimeError("console closed before shell prompt")
                data += chunk
                if b"terminal already attached" in data:
                    raise RuntimeError("terminal already attached; detach with ~. first")
                if re.search(SHELL_PROMPT_PATTERN, data):
                    break
            else:
                raise TimeoutError("shell prompt timeout")
            gate.wait(timeout=max(1.0, deadline - time.monotonic()))
            stream.sendall(b"\x15/usr/bin/ubsim-checkpoint\n")
            while time.monotonic() < deadline:
                try:
                    if not stream.recv(65536):
                        return
                except socket.timeout:
                    continue
            raise TimeoutError("gem5 did not stop after checkpoint")
    except Exception as error:  # report every node, not only the first one
        errors.append(f"UART {port}: {error}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ports", nargs="+", type=int, required=True)
    parser.add_argument("--timeout", type=float, default=600)
    args = parser.parse_args()
    gate = threading.Barrier(len(args.ports))
    errors: list[str] = []
    threads = [
        threading.Thread(target=checkpoint_one,
                         args=(port, args.timeout, gate, errors), daemon=True)
        for port in args.ports
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(args.timeout + 10)
    if errors:
        for error in errors:
            print(error)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
