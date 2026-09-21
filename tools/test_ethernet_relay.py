#!/usr/bin/env python3
"""Three-port broadcast and MAC-learning test for ethernet_relay.py."""

from __future__ import annotations

import socket
import struct
import threading

from ethernet_relay import receive_port, recv_exact


def packet(destination: bytes, source: bytes, payload: bytes) -> bytes:
    frame = destination + source + b"\x08\x00" + payload
    return struct.pack("!I", len(frame)) + frame


def receive(sock: socket.socket) -> bytes:
    (length,) = struct.unpack("!I", recv_exact(sock, 4))
    return recv_exact(sock, length)


def main() -> int:
    pairs = [socket.socketpair() for _ in range(3)]
    switch_ports = [pair[0] for pair in pairs]
    hosts = [pair[1] for pair in pairs]
    stopped = threading.Event()
    send_locks = [threading.Lock() for _ in switch_ports]
    mac_table: dict[bytes, int] = {}
    table_lock = threading.Lock()
    workers = [
        threading.Thread(
            target=receive_port,
            args=(index, sock, switch_ports, send_locks,
                  mac_table, table_lock, stopped),
            daemon=True,
        )
        for index, sock in enumerate(switch_ports)
    ]
    for worker in workers:
        worker.start()

    mac0 = bytes.fromhex("020000000001")
    mac2 = bytes.fromhex("020000000003")
    hosts[0].sendall(packet(b"\xff" * 6, mac0, b"broadcast"))
    assert receive(hosts[1]).endswith(b"broadcast")
    assert receive(hosts[2]).endswith(b"broadcast")

    hosts[2].sendall(packet(mac0, mac2, b"learn-node2"))
    assert receive(hosts[0]).endswith(b"learn-node2")
    hosts[0].sendall(packet(mac2, mac0, b"known-unicast"))
    assert receive(hosts[2]).endswith(b"known-unicast")
    hosts[1].settimeout(0.05)
    try:
        hosts[1].recv(1)
        raise AssertionError("known unicast was flooded to an unrelated port")
    except TimeoutError:
        pass

    stopped.set()
    for sock in hosts + switch_ports:
        sock.close()
    print("ethernet relay learning-switch test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
