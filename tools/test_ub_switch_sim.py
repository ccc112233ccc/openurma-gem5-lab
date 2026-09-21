#!/usr/bin/env python3
"""Deterministic ABI/forwarding smoke test for ub-switch-sim."""

import mmap
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import time


PORTS = 1
RING_SLOTS = 64
SLOT_BYTES = 8192
RING_HEADER = 4096
RING_BYTES = RING_HEADER + 2 * PORTS * RING_SLOTS * SLOT_BYTES
MESSAGE = struct.Struct("<IIQQQHHHH24s")


def index_offset(direction: int, port: int, tail: bool = False) -> int:
    return ((direction * 16 + port) * 128) + (64 if tail else 0)


def slot_offset(direction: int, port: int, index: int) -> int:
    queue = direction * PORTS + port
    return RING_HEADER + (queue * RING_SLOTS + index % RING_SLOTS) * SLOT_BYTES


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} UB_SWITCH_BINARY", file=sys.stderr)
        return 2
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="openurma-adapter-") as directory:
        paths = [pathlib.Path(directory) / f"node{node}.ring" for node in range(2)]
        maps = []
        for path in paths:
            with path.open("wb") as output:
                output.truncate(RING_BYTES)
            descriptor = os.open(path, os.O_RDWR)
            maps.append(mmap.mmap(descriptor, RING_BYTES))
            os.close(descriptor)

        process = subprocess.Popen(
            [str(binary), str(paths[0]), str(paths[1]), "1", "100t", "50t",
             "400", "0", "", "1"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            payload = bytes(range(128))
            transaction = bytes(40) + payload
            header = MESSAGE.pack(
                len(transaction), 1, 1000, 1100, 7, 0, 0, 1, 0, bytes(24)
            )
            offset = slot_offset(1, 0, 0)
            maps[0][offset : offset + MESSAGE.size] = header
            maps[0][offset + MESSAGE.size : offset + MESSAGE.size + len(transaction)] = transaction
            struct.pack_into("<Q", maps[0], index_offset(1, 0), 1)

            deadline = time.monotonic() + 2
            while struct.unpack_from("<Q", maps[1], index_offset(0, 0))[0] != 1:
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read())
                if time.monotonic() >= deadline:
                    raise RuntimeError("switch did not forward DATA")
                time.sleep(0.001)

            received = MESSAGE.unpack_from(maps[1], slot_offset(0, 0, 0))
            # 1100 ingress arrival + 50 switch + ceil(128*8000/400)
            # egress serialization + 100 egress propagation.
            assert received[3] == 3810, received
            assert received[4] == 7
            assert maps[1][slot_offset(0, 0, 0) + MESSAGE.size:
                           slot_offset(0, 0, 0) + MESSAGE.size + len(transaction)] == transaction
            assert struct.unpack_from("<Q", maps[0], index_offset(1, 0, True))[0] == 1
        finally:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            for mapping in maps:
                mapping.close()
    print("ub-switch adapter smoke test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
