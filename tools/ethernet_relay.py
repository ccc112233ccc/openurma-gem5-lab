#!/usr/bin/env python3
"""Relay frames between two gem5 EtherTapStub sockets.

EtherTapStub prefixes each raw Ethernet frame with a four-byte, network-order
length.  The relay preserves that framing and never interprets the packet.
"""

from __future__ import annotations

import argparse
import socket
import struct
import threading
import time


MAX_FRAME = 65536


def connect(endpoint: str, deadline: float) -> socket.socket:
    while True:
        sock: socket.socket | None = None
        try:
            if endpoint.startswith("unix:"):
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(endpoint.removeprefix("unix:"))
            elif endpoint.startswith("tcp:"):
                host, port = endpoint.removeprefix("tcp:").rsplit(":", 1)
                sock = socket.create_connection((host, int(port)), timeout=1.0)
                sock.settimeout(None)
            else:
                raise ValueError(
                    f"unsupported endpoint {endpoint!r}; use unix:PATH or tcp:HOST:PORT"
                )
            return sock
        except (FileNotFoundError, ConnectionError, OSError):
            if sock is not None:
                sock.close()
            if time.monotonic() >= deadline:
                raise TimeoutError(f"timed out connecting to {endpoint}")
            time.sleep(0.1)


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("peer closed")
        data.extend(chunk)
    return bytes(data)


def forward(
    label: str,
    source: socket.socket,
    destination: socket.socket,
    stopped: threading.Event,
) -> None:
    try:
        while not stopped.is_set():
            header = recv_exact(source, 4)
            (length,) = struct.unpack("!I", header)
            if length < 14 or length > MAX_FRAME:
                raise ValueError(f"invalid Ethernet frame length {length}")
            destination.sendall(header + recv_exact(source, length))
    except (EOFError, OSError, ValueError) as error:
        if not stopped.is_set():
            print(f"[ethernet-relay] {label}: {error}", flush=True)
    finally:
        stopped.set()
        for sock in (source, destination):
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("endpoint0", help="unix:PATH or tcp:HOST:PORT")
    parser.add_argument("endpoint1", help="unix:PATH or tcp:HOST:PORT")
    parser.add_argument("--connect-timeout", type=float, default=300.0)
    args = parser.parse_args()

    deadline = time.monotonic() + args.connect_timeout
    left = connect(args.endpoint0, deadline)
    print(f"[ethernet-relay] connected {args.endpoint0}", flush=True)
    right = connect(args.endpoint1, deadline)
    print(f"[ethernet-relay] connected {args.endpoint1}", flush=True)

    stopped = threading.Event()
    workers = [
        threading.Thread(
            target=forward, args=("node0 -> node1", left, right, stopped)
        ),
        threading.Thread(
            target=forward, args=("node1 -> node0", right, left, stopped)
        ),
    ]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()
    left.close()
    right.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
