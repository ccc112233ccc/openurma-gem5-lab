#!/usr/bin/env python3
"""Read an AArch64 PC from gem5's remote-GDB stub, then detach cleanly."""

import socket
import sys


def packet(payload: str) -> bytes:
    raw = payload.encode("ascii")
    return b"$" + raw + b"#" + f"{sum(raw) & 0xff:02x}".encode("ascii")


def receive(sock: socket.socket) -> str:
    while True:
        char = sock.recv(1)
        if not char:
            raise EOFError("remote closed the RSP connection")
        if char == b"$":
            break
    data = bytearray()
    while True:
        char = sock.recv(1)
        if char == b"#":
            break
        data.extend(char)
    sock.recv(2)
    sock.sendall(b"+")
    return data.decode("ascii")


def request(sock: socket.socket, payload: str) -> str:
    sock.sendall(packet(payload))
    first = sock.recv(1)
    if first != b"+":
        raise RuntimeError(f"missing RSP acknowledgement: {first!r}")
    return receive(sock)


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7000
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        stop = request(sock, "?")
        registers = bytes.fromhex(request(sock, "g"))
        if len(registers) < 264:
            raise RuntimeError(f"short AArch64 register packet: {len(registers)} bytes")
        sp = int.from_bytes(registers[248:256], "little")
        pc = int.from_bytes(registers[256:264], "little")
        insn = request(sock, f"m{pc:x},10")
        print(f"stop={stop} pc=0x{pc:016x} sp=0x{sp:016x} bytes={insn}",
              flush=True)
        sock.sendall(packet("D"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
