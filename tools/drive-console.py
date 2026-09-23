#!/usr/bin/env python3
"""Run one shell command through a gem5 terminal TCP port and report its rc."""

import argparse
import socket
import sys
import time
import uuid


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("port", type=int)
    parser.add_argument("command")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--timeout", type=float, default=60.0)
    args = parser.parse_args()
    marker = f"__OPENURMA_RC_{uuid.uuid4().hex}__"
    deadline = time.monotonic() + args.timeout
    payload = f"\n{args.command}; printf '\\n{marker}%s\\n' $?\n".encode()
    received = bytearray()

    with socket.create_connection((args.host, args.port), timeout=args.timeout) as sock:
        sock.settimeout(0.5)
        sock.sendall(payload)
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                break
            received.extend(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            start = received.find(marker.encode())
            if start >= 0:
                tail = received[start + len(marker):].splitlines()[0]
                try:
                    return int(tail)
                except ValueError:
                    return 3
    print(f"drive-console.py: no exit marker before timeout: {marker}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
