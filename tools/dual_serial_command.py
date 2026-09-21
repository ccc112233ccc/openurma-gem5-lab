#!/usr/bin/env python3
"""Run commands concurrently on gem5 UARTs and await fresh prompts."""

from __future__ import annotations

import argparse
import re
import socket
import threading
import time


def run_one(
    port: int,
    command: str,
    timeout: float,
    start_delay: float,
    start_gate: threading.Barrier,
    results: dict[int, str],
) -> None:
    deadline = time.monotonic() + timeout
    chunks: list[bytes] = []
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=5.0) as sock:
            sock.settimeout(0.2)
            prompt_pattern = rb"\(openurma-[^)]+\)[^\r\n]*# "
            # A freshly started full-system guest can take minutes of host
            # time to reach its shell while conservative dist synchronization
            # is active.  Never inject into the boot stream: bytes sent before
            # ash owns the console are replayed later as a partial command.
            warm_deadline = deadline
            prompt_ready = False
            while time.monotonic() < warm_deadline:
                try:
                    data = sock.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    break
                chunks.append(data)
                if b"terminal already attached" in data:
                    raise RuntimeError("terminal already attached; detach it with ~. first")
                if re.search(prompt_pattern, b"".join(chunks)) is not None:
                    prompt_ready = True
                    break
            if not prompt_ready:
                raise TimeoutError(f"no shell prompt before command within {timeout:g}s")

            # Both consoles are connected and drained before either command is
            # injected.  This keeps a host scheduling difference from turning
            # into a large guest virtual-time skew before dist sync is enabled.
            try:
                start_gate.wait(timeout=max(0.1, deadline - time.monotonic()))
            except threading.BrokenBarrierError as exc:
                raise RuntimeError("peer UART did not reach the command start gate") from exc
            if start_delay:
                time.sleep(start_delay)
            pre_send_len = len(b"".join(chunks))
            status_marker = (
                f"__OPENURMA_RC_{port}_{time.monotonic_ns()}__".encode()
            )
            wrapped = (
                command.encode()
                + b"; __ou_rc=$?; printf '\\n"
                + status_marker
                + b"=%d\\n' \"$__ou_rc\"\n"
            )
            sock.sendall(b"\x15" + wrapped)
            while time.monotonic() < deadline:
                try:
                    data = sock.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    break
                chunks.append(data)
                text = b"".join(chunks)
                new_text = text[pre_send_len:]
                status_match = re.search(
                    rb"(?:\r?\n)" + re.escape(status_marker) + rb"=([0-9]+)\r?\n",
                    new_text,
                )
                if status_match is not None:
                    prompt_seen = re.search(
                        prompt_pattern,
                        new_text[status_match.end() :],
                    ) is not None
                    if prompt_seen:
                        # Exclude bytes drained before this command. This keeps
                        # repeated sweeps from accidentally re-parsing an old
                        # result that was still buffered on the UART socket.
                        transcript = new_text.decode(errors="replace")
                        status = int(status_match.group(1))
                        if status != 0:
                            results[port] = (
                                f"ERROR: command exited with status {status}\n"
                                + transcript
                            )
                        else:
                            results[port] = transcript
                        return
            raise TimeoutError(f"no shell prompt after collective command within {timeout:g}s")
    except Exception as exc:
        tail = b"".join(chunks)[-1000:].decode(errors="replace")
        results[port] = f"ERROR: {exc}\n{tail}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ports", type=int, nargs="+", required=True)
    command_group = parser.add_mutually_exclusive_group(required=True)
    command_group.add_argument("--command")
    command_group.add_argument(
        "--commands",
        nargs="+",
        metavar="COMMAND",
        help="send one corresponding command to each port",
    )
    parser.add_argument(
        "--stagger",
        type=float,
        default=0.0,
        help=(
            "delay node1 after the shared start gate by this many host seconds "
            "(normally keep zero for virtual-time experiments)"
        ),
    )
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument(
        "--full-output",
        action="store_true",
        help="print the complete current-command transcript instead of its tail",
    )
    args = parser.parse_args()

    results: dict[int, str] = {}
    if args.commands is not None and len(args.commands) != len(args.ports):
        parser.error("--commands must provide exactly one command per port")
    commands = (
        args.commands
        if args.commands is not None
        else [args.command] * len(args.ports)
    )
    start_gate = threading.Barrier(len(args.ports))
    threads = [
        threading.Thread(
            target=run_one,
            args=(
                port,
                commands[index],
                args.timeout,
                args.stagger if index else 0.0,
                start_gate,
                results,
            ),
            daemon=True,
        )
        for index, port in enumerate(args.ports)
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(args.timeout + 6.0)

    failed = False
    for port in args.ports:
        transcript = results.get(port, "ERROR: worker did not finish")
        print(f"--- UART {port} ---")
        is_error = transcript.startswith("ERROR:")
        if is_error:
            headline, _, body = transcript.partition("\n")
            print(headline)
            print(body[-1200:].rstrip())
        else:
            shown = transcript if args.full_output else transcript[-1200:]
            print(shown.rstrip())
        failed |= is_error
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
