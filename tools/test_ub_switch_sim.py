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
RING_HEADER = 8192
RING_BYTES = RING_HEADER + 2 * PORTS * RING_SLOTS * SLOT_BYTES
MESSAGE = struct.Struct("<IIQQQHHHHII16s")


def index_offset(direction: int, port: int, tail: bool = False) -> int:
    return ((direction * 16 + port) * 128) + (64 if tail else 0)


def slot_offset(direction: int, port: int, index: int) -> int:
    queue = direction * PORTS + port
    return RING_HEADER + (queue * RING_SLOTS + index % RING_SLOTS) * SLOT_BYTES


def create_maps(directory: str, count: int) -> tuple[list[pathlib.Path], list[mmap.mmap]]:
    paths = [pathlib.Path(directory) / f"node{node}.ring" for node in range(count)]
    maps = []
    for path in paths:
        with path.open("wb") as output:
            output.truncate(RING_BYTES)
        descriptor = os.open(path, os.O_RDWR)
        maps.append(mmap.mmap(descriptor, RING_BYTES))
        os.close(descriptor)
    return paths, maps


def wait_for(predicate, process: subprocess.Popen[str], reason: str) -> None:
    deadline = time.monotonic() + 2
    while not predicate():
        if process.poll() is not None:
            raise RuntimeError(process.stderr.read())
        if time.monotonic() >= deadline:
            raise RuntimeError(reason)
        time.sleep(0.001)


def run_four_endpoint_test(binary: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="openurma-adapter-four-") as directory:
        paths, maps = create_maps(directory, 4)
        process = subprocess.Popen(
            [str(binary), "--multi", "1", "100t", "50t", "400", "0",
             "", "1", "0x100,0x101,0x102,0x103",
             *(str(path) for path in paths)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            # Endpoints 0 and 3 choose each other by EID. This deliberately
            # crosses the old adjacent-pair topology.
            struct.pack_into("<I", maps[0], 4116, 0x103)
            struct.pack_into("<I", maps[3], 4116, 0x100)
            struct.pack_into("<Q", maps[0], 4096, 1)
            struct.pack_into("<Q", maps[3], 4096, 1)
            wait_for(
                lambda: struct.unpack_from("<Q", maps[0], 4104)[0] == 1 and
                        struct.unpack_from("<Q", maps[3], 4104)[0] == 1,
                process,
                "four-endpoint switch did not route sync phases by EID",
            )
            assert struct.unpack_from("<Q", maps[1], 4104)[0] == 0
            assert struct.unpack_from("<Q", maps[2], 4104)[0] == 0

            payload = bytes(range(64))
            transaction = bytes(40) + payload
            header = MESSAGE.pack(
                len(transaction), 1, 2000, 2100, 19, 0, 0, 3, 0,
                0x100, 0x103, bytes(16)
            )
            offset = slot_offset(1, 0, 0)
            maps[0][offset : offset + MESSAGE.size] = header
            maps[0][offset + MESSAGE.size :
                    offset + MESSAGE.size + len(transaction)] = transaction
            struct.pack_into("<Q", maps[0], index_offset(1, 0), 1)
            wait_for(
                lambda: struct.unpack_from(
                    "<Q", maps[3], index_offset(0, 0))[0] == 1,
                process,
                "four-endpoint switch did not route DATA by destination EID",
            )
            assert struct.unpack_from("<Q", maps[1], index_offset(0, 0))[0] == 0
            assert struct.unpack_from("<Q", maps[2], index_offset(0, 0))[0] == 0
            received = MESSAGE.unpack_from(maps[3], slot_offset(0, 0, 0))
            # 2100 ingress arrival + 50 switch + ceil(64*8000/400)
            # egress serialization + 100 egress propagation.
            assert received[3] == 3530, received

            # Local phase counters need not match after endpoints have run
            # different prior sessions. The switch acknowledges each side's
            # own generation once the selected endpoints are reciprocal.
            struct.pack_into("<Q", maps[0], 4096, 2)
            struct.pack_into("<Q", maps[3], 4096, 2)
            wait_for(
                lambda: struct.unpack_from("<Q", maps[0], 4104)[0] == 2 and
                        struct.unpack_from("<Q", maps[3], 4104)[0] == 2,
                process,
                "four-endpoint switch did not acknowledge OFF",
            )
            struct.pack_into("<I", maps[0], 4116, 0x102)
            struct.pack_into("<I", maps[2], 4116, 0x100)
            struct.pack_into("<Q", maps[0], 4096, 5)
            struct.pack_into("<Q", maps[2], 4096, 1)
            wait_for(
                lambda: struct.unpack_from("<Q", maps[0], 4104)[0] == 5 and
                        struct.unpack_from("<Q", maps[2], 4104)[0] == 1,
                process,
                "different local synchronization generations did not rendezvous",
            )
        finally:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            for mapping in maps:
                mapping.close()


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} UB_SWITCH_BINARY", file=sys.stderr)
        return 2
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="openurma-adapter-") as directory:
        paths, maps = create_maps(directory, 2)

        process = subprocess.Popen(
            [str(binary), str(paths[0]), str(paths[1]), "1", "100t", "50t",
             "400", "0", "", "1"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            struct.pack_into("<I", maps[0], 4116, 0x101)
            struct.pack_into("<I", maps[1], 4116, 0x100)
            struct.pack_into("<Q", maps[0], 4096, 1)
            struct.pack_into("<Q", maps[1], 4096, 1)
            deadline = time.monotonic() + 2
            while struct.unpack_from("<Q", maps[1], 4104)[0] != 1:
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read())
                if time.monotonic() >= deadline:
                    raise RuntimeError("switch did not mirror sync phase")
                time.sleep(0.001)
            payload = bytes(range(128))
            transaction = bytes(40) + payload
            header = MESSAGE.pack(
                len(transaction), 1, 1000, 1100, 7, 0, 0, 3, 0,
                0x100, 0x101, bytes(16)
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

            # A null-message promise follows DATA on the same FIFO and may
            # never move the output timestamp backwards behind queued data.
            sync_offset = slot_offset(1, 0, 1)
            maps[0][sync_offset : sync_offset + MESSAGE.size] = MESSAGE.pack(
                0, 2, 1100, 1200, 8, 0, 0, 3, 0,
                0x100, 0x101, bytes(16)
            )
            struct.pack_into("<Q", maps[0], index_offset(1, 0), 2)
            deadline = time.monotonic() + 2
            while struct.unpack_from("<Q", maps[1], index_offset(0, 0))[0] != 2:
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read())
                if time.monotonic() >= deadline:
                    raise RuntimeError("switch did not forward SYNC")
                time.sleep(0.001)
            sync = MESSAGE.unpack_from(maps[1], slot_offset(0, 0, 1))
            assert sync[0] == 0 and sync[1] == 2
            assert sync[3] == 3810, sync
        finally:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            for mapping in maps:
                mapping.close()
    run_four_endpoint_test(binary)
    print("ub-switch adapter smoke test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
