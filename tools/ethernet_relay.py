#!/usr/bin/env python3
"""Learning Ethernet switch for gem5 EtherTapStub sockets.

EtherTapStub prefixes each raw Ethernet frame with a four-byte, network-order
length. The relay learns source MAC addresses, unicasts known destinations,
and floods broadcasts, multicasts, and unknown destinations.
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


def receive_port(
    source_index: int,
    source: socket.socket,
    sockets: list[socket.socket],
    send_locks: list[threading.Lock],
    mac_table: dict[bytes, int],
    table_lock: threading.Lock,
    stopped: threading.Event,
) -> None:
    try:
        while not stopped.is_set():
            header = recv_exact(source, 4)
            (length,) = struct.unpack("!I", header)
            if length < 14 or length > MAX_FRAME:
                raise ValueError(f"invalid Ethernet frame length {length}")
            frame = recv_exact(source, length)
            destination_mac = frame[:6]
            source_mac = frame[6:12]
            with table_lock:
                if not (source_mac[0] & 1):
                    mac_table[source_mac] = source_index
                destination_index = mac_table.get(destination_mac)
            if destination_mac[0] & 1 or destination_index is None:
                destinations = [
                    index for index in range(len(sockets))
                    if index != source_index
                ]
            elif destination_index == source_index:
                destinations = []
            else:
                destinations = [destination_index]
            packet = header + frame
            for index in destinations:
                with send_locks[index]:
                    sockets[index].sendall(packet)
    except (EOFError, OSError, ValueError) as error:
        if not stopped.is_set():
            print(f"[ethernet-relay] port {source_index}: {error}", flush=True)
    finally:
        stopped.set()
        for sock in sockets:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "endpoints", nargs="+",
        help="two or more unix:PATH or tcp:HOST:PORT endpoints",
    )
    parser.add_argument("--connect-timeout", type=float, default=300.0)
    args = parser.parse_args()
    if len(args.endpoints) < 2:
        parser.error("at least two endpoints are required")

    deadline = time.monotonic() + args.connect_timeout
    sockets = []
    for endpoint in args.endpoints:
        sockets.append(connect(endpoint, deadline))
        print(f"[ethernet-relay] connected {endpoint}", flush=True)

    stopped = threading.Event()
    send_locks = [threading.Lock() for _ in sockets]
    mac_table: dict[bytes, int] = {}
    table_lock = threading.Lock()
    workers = [
        threading.Thread(
            target=receive_port,
            args=(index, sock, sockets, send_locks, mac_table, table_lock, stopped),
        )
        for index, sock in enumerate(sockets)
    ]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()
    for sock in sockets:
        sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
