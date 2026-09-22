#!/usr/bin/env python3
"""Focused native UbSwitch/UbPort/UbLink adapter regression."""

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
MESSAGE = struct.Struct("<IIQQQHHHHIIQ8s")


def index_offset(direction: int, tail: bool = False) -> int:
    return direction * 16 * 128 + (64 if tail else 0)


def slot_offset(direction: int, index: int) -> int:
    return RING_HEADER + (direction * RING_SLOTS + index % RING_SLOTS) * SLOT_BYTES


def wait_for(predicate, process: subprocess.Popen[str], reason: str) -> None:
    deadline = time.monotonic() + 3
    while not predicate():
        if process.poll() is not None:
            raise RuntimeError(process.stderr.read())
        if time.monotonic() >= deadline:
            raise RuntimeError(reason)
        time.sleep(0.001)


def put_record(mapping: mmap.mmap, index: int, fields: tuple, payload: bytes = b"") -> None:
    offset = slot_offset(1, index)
    mapping[offset : offset + MESSAGE.size] = MESSAGE.pack(*fields)
    mapping[offset + MESSAGE.size : offset + MESSAGE.size + len(payload)] = payload


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} UB_ADAPTER_BINARY", file=sys.stderr)
        return 2
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="openurma-native-adapter-") as directory:
        paths = [pathlib.Path(directory) / f"node{node}.ring" for node in range(2)]
        maps = []
        for path in paths:
            with path.open("wb") as output:
                output.truncate(RING_BYTES)
            descriptor = os.open(path, os.O_RDWR)
            maps.append(mmap.mmap(descriptor, RING_BYTES))
            os.close(descriptor)

        process = subprocess.Popen(
            [
                str(binary),
                "--native-multi",
                "1",
                "100ps",
                "0ps",
                "400",
                "0",
                "",
                "1",
                "0x100,0x101",
                *(str(path) for path in paths),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            struct.pack_into("<I", maps[0], 4116, 0x101)
            struct.pack_into("<I", maps[1], 4116, 0x100)
            struct.pack_into("<Q", maps[0], 4096, 1)
            struct.pack_into("<Q", maps[1], 4096, 1)
            wait_for(
                lambda: struct.unpack_from("<Q", maps[0], 4104)[0] == 1
                and struct.unpack_from("<Q", maps[1], 4104)[0] == 1,
                process,
                "native adapter did not acknowledge the first active generation",
            )

            payload = bytes(40) + bytes(range(64))
            put_record(
                maps[0],
                0,
                (len(payload), 1, 900, 1000, 7, 0, 0, 3, 0, 0x100, 0x101, 0, bytes(8)),
                payload,
            )
            put_record(
                maps[0],
                1,
                (0, 2, 1000, 50000, 8, 0, 0, 3, 0, 0x100, 0x101, 1, bytes(8)),
            )
            put_record(
                maps[1],
                0,
                (0, 2, 1000, 50000, 9, 0, 0, 3, 0, 0x101, 0x100, 1, bytes(8)),
            )
            struct.pack_into("<Q", maps[0], index_offset(1), 2)
            struct.pack_into("<Q", maps[1], index_offset(1), 1)
            wait_for(
                lambda: struct.unpack_from("<Q", maps[1], index_offset(0))[0] >= 2,
                process,
                "native UbSwitch path did not deliver DATA and a safe-time grant",
            )
            received = MESSAGE.unpack_from(maps[1], slot_offset(0, 0))
            assert received[1] == 1, received
            assert received[2] == 900, received
            # Native arrival includes the production allocator, egress-port
            # serialization of real UB headers, and UbLink propagation.  Do
            # not freeze their current defaults into this adapter test.
            assert 1100 < received[3] < 50000, received
            assert received[4] == 7, received
            assert received[5] == 0 and received[6] == 0, received
            assert received[9] == 0x100 and received[10] == 0x101, received
            offset = slot_offset(0, 0) + MESSAGE.size
            assert maps[1][offset : offset + len(payload)] == payload

            struct.pack_into("<Q", maps[0], 4096, 2)
            struct.pack_into("<Q", maps[1], 4096, 2)
            wait_for(
                lambda: struct.unpack_from("<Q", maps[0], 4104)[0] == 2
                and struct.unpack_from("<Q", maps[1], 4104)[0] == 2,
                process,
                "native adapter did not acknowledge the first OFF generation",
            )

            struct.pack_into("<Q", maps[0], 4096, 3)
            wait_for(
                lambda: struct.unpack_from("<Q", maps[0], 4104)[0] == 2,
                process,
                "native adapter did not hold an early second-generation endpoint",
            )
            # The early endpoint may publish and have its first promise
            # consumed before its peer enters the generation. The adapter must
            # retire a stale prior-generation promise and retain the new one
            # rather than comparing timestamps from different wire epochs.
            put_record(
                maps[0],
                2,
                (0, 2, 50000, 60000, 10, 0, 0, 3, 0, 0x100, 0x101, 1, bytes(8)),
            )
            put_record(
                maps[0],
                3,
                (0, 2, 50000, 100000, 11, 0, 0, 3, 0, 0x100, 0x101, 3, bytes(8)),
            )
            struct.pack_into("<Q", maps[0], index_offset(1), 4)
            wait_for(
                lambda: struct.unpack_from("<Q", maps[0], index_offset(1, tail=True))[0]
                == 4,
                process,
                "native adapter did not consume the early generation promise",
            )
            struct.pack_into("<Q", maps[1], 4096, 3)
            wait_for(
                lambda: struct.unpack_from("<Q", maps[0], 4104)[0] == 3
                and struct.unpack_from("<Q", maps[1], 4104)[0] == 3,
                process,
                "native adapter did not rendezvous the second generation",
            )

            put_record(
                maps[1],
                1,
                (0, 2, 50000, 100000, 12, 0, 0, 3, 0, 0x101, 0x100, 3, bytes(8)),
            )
            struct.pack_into("<Q", maps[1], index_offset(1), 2)
            try:
                wait_for(
                    lambda: struct.unpack_from("<Q", maps[0], index_offset(0))[0] >= 2
                    and struct.unpack_from("<Q", maps[1], index_offset(0))[0] >= 3,
                    process,
                    "native adapter did not publish the second safe-time grant",
                )
            except RuntimeError as error:
                state = [
                    {
                        "out_head": struct.unpack_from("<Q", mapping, index_offset(0))[0],
                        "in_head": struct.unpack_from("<Q", mapping, index_offset(1))[0],
                        "in_tail": struct.unpack_from(
                            "<Q", mapping, index_offset(1, tail=True)
                        )[0],
                        "local_phase": struct.unpack_from("<Q", mapping, 4096)[0],
                        "peer_phase": struct.unpack_from("<Q", mapping, 4104)[0],
                    }
                    for mapping in maps
                ]
                process.terminate()
                _, diagnostics = process.communicate(timeout=2)
                raise RuntimeError(
                    f"{error}; ring state={state}; adapter log={diagnostics}"
                ) from error
            struct.pack_into("<Q", maps[0], 4096, 4)
            struct.pack_into("<Q", maps[1], 4096, 4)
            wait_for(
                lambda: struct.unpack_from("<Q", maps[0], 4104)[0] == 4
                and struct.unpack_from("<Q", maps[1], 4104)[0] == 4,
                process,
                "native adapter did not finish the second OFF generation",
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
    print("ns-3-UB native adapter test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
