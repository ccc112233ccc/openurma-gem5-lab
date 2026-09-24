#!/usr/bin/env python3
"""Deterministic lifetime-sync/forwarding smoke test for ub-switch-sim."""

import mmap
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import time


RING_SLOTS = 64
SLOT_BYTES = 8192
RING_HEADER = 8192
PROTOCOL_VERSION = 4
DATA = 1
SYNC = 2
MESSAGE = struct.Struct("<IIQQQHHHHII16s")


def index_offset(direction: int, port: int, tail: bool = False) -> int:
    return ((direction * 16 + port) * 128) + (64 if tail else 0)


def slot_offset(ports: int, direction: int, port: int, index: int) -> int:
    queue = direction * ports + port
    return RING_HEADER + (queue * RING_SLOTS + index % RING_SLOTS) * SLOT_BYTES


def create_maps(directory: str, count: int, ports: int) -> tuple[list[pathlib.Path], list[mmap.mmap]]:
    paths = [pathlib.Path(directory) / f"node{node}.ring" for node in range(count)]
    maps = []
    for path in paths:
        with path.open("wb") as output:
            output.truncate(RING_HEADER + 2 * ports * RING_SLOTS * SLOT_BYTES)
        descriptor = os.open(path, os.O_RDWR)
        maps.append(mmap.mmap(descriptor, 0))
        os.close(descriptor)
    return paths, maps


def wait_for(predicate, process: subprocess.Popen[str], reason: str) -> None:
    deadline = time.monotonic() + 5
    while not predicate():
        if process.poll() is not None:
            raise RuntimeError(process.stderr.read())
        if time.monotonic() >= deadline:
            raise RuntimeError(reason)
        time.sleep(0.001)


def publish(mapping: mmap.mmap, ports: int, port: int,
            receive_tick: int, sequence: int,
            source_eid: int = 0, destination_eid: int = 0,
            payload: bytes | None = None) -> None:
    direction = 1
    head = struct.unpack_from("<Q", mapping, index_offset(direction, port))[0]
    body = b"" if payload is None else bytes(40) + payload
    message_type = SYNC if payload is None else DATA
    header = MESSAGE.pack(
        len(body), message_type, max(0, receive_tick - 100), receive_tick,
        sequence, port, port, PROTOCOL_VERSION, 0, source_eid,
        destination_eid, bytes(16),
    )
    offset = slot_offset(ports, direction, port, head)
    mapping[offset:offset + MESSAGE.size] = header
    mapping[offset + MESSAGE.size:offset + MESSAGE.size + len(body)] = body
    struct.pack_into("<Q", mapping, index_offset(direction, port), head + 1)


def output_messages(mapping: mmap.mmap, ports: int, port: int) -> list[tuple]:
    head = struct.unpack_from("<Q", mapping, index_offset(0, port))[0]
    return [MESSAGE.unpack_from(mapping, slot_offset(ports, 0, port, index))
            for index in range(head)]


def run_case(binary: pathlib.Path, endpoints: int, ports: int, mode: str) -> None:
    with tempfile.TemporaryDirectory(prefix=f"openurma-adapter-{endpoints}x{ports}-") as directory:
        paths, maps = create_maps(directory, endpoints, ports)
        eids = ",".join(hex(0x100 + endpoint) for endpoint in range(endpoints))
        process = subprocess.Popen(
            [str(binary), mode, str(ports), "100t", "50t", "400", "0",
             "", "1", eids, *(str(path) for path in paths)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        try:
            # Endpoints publish their first link promise at simulation start.
            # No EID, TP, workload, or reciprocal peer relation is needed.
            for endpoint, mapping in enumerate(maps):
                for port in range(ports):
                    publish(mapping, ports, port, 100,
                            endpoint * ports + port + 1)
            wait_for(
                lambda: all(len(output_messages(mapping, ports, port)) >= 1
                            for mapping in maps for port in range(ports)),
                process, "switch virtual time did not advance on all-link grants",
            )
            for mapping in maps:
                for port in range(ports):
                    first = output_messages(mapping, ports, port)[0]
                    expected_first_grant = 200 if mode == "--multi" else 100
                    assert first[1] == SYNC and first[3] == expected_first_grant, first
                    assert first[5] == port and first[6] == port, first
                    assert first[9] == 0 and first[10] == 0, first

            payload = bytes(range(128))
            publish(maps[0], ports, 0, 300, 100, 0x100,
                    0x100 + endpoints - 1, payload)
            for endpoint, mapping in enumerate(maps):
                for port in range(ports):
                    if endpoint == 0 and port == 0:
                        continue
                    publish(mapping, ports, port, 300,
                            100 + endpoint * ports + port)
            if mode == "--native-multi":
                # Native DATA remains an ns-3 event after switch ingress.
                # Supply the next all-link promise so the fabric can advance
                # far enough to execute its egress/link delivery event.
                for endpoint, mapping in enumerate(maps):
                    for port in range(ports):
                        publish(mapping, ports, port, 10000,
                                1000 + endpoint * ports + port)
            destination = maps[-1]
            wait_for(
                lambda: any(message[1] == DATA for message in
                            output_messages(destination, ports, 0)),
                process, "switch did not route causally-ready DATA by EID",
            )
            data_index, received = next(
                (index, message) for index, message in
                enumerate(output_messages(destination, ports, 0))
                if message[1] == DATA
            )
            if mode == "--multi":
                assert received[3] == 3010, received
            else:
                assert received[3] > 300, received
            offset = slot_offset(ports, 0, 0, data_index) + MESSAGE.size + 40
            assert destination[offset:offset + len(payload)] == payload
            for mapping in maps[1:-1]:
                assert not any(message[1] == DATA for message in
                               output_messages(mapping, ports, 0))

            wait_for(
                lambda: len(output_messages(destination, ports, 0)) >
                        data_index + 1,
                process, "switch did not publish post-DATA promise",
            )
            after = output_messages(destination, ports, 0)[data_index + 1]
            assert after[1] == SYNC and after[3] >= received[3], after
        finally:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            for mapping in maps:
                mapping.close()


def run_unsynchronized_case(binary: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="openurma-adapter-unsync-") as directory:
        paths, maps = create_maps(directory, 2, 1)
        process = subprocess.Popen(
            [str(binary), "--unsynchronized", "--multi", "1", "100t", "50t",
             "400", "0", "", "1", "0x100,0x101", *(str(path) for path in paths)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        try:
            payload = bytes(range(64))
            # No null-message promise is published. Functional mode must still
            # forward DATA while preserving the modeled arrival timestamp.
            publish(maps[0], 1, 0, 300, 1, 0x100, 0x101, payload)
            wait_for(
                lambda: any(message[1] == DATA
                            for message in output_messages(maps[1], 1, 0)),
                process, "unsynchronized switch waited for a SYNC promise",
            )
            messages = output_messages(maps[1], 1, 0)
            assert all(message[1] != SYNC for message in messages), messages
            received = next(message for message in messages if message[1] == DATA)
            assert received[3] == 1730, received
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
    if len(sys.argv) not in (2, 3):
        print(f"usage: {sys.argv[0]} UB_SWITCH_BINARY [--multi|--native-multi]",
              file=sys.stderr)
        return 2
    binary = pathlib.Path(sys.argv[1]).resolve()
    mode = sys.argv[2] if len(sys.argv) == 3 else "--multi"
    if mode not in ("--multi", "--native-multi"):
        raise ValueError(f"unsupported switch mode: {mode}")
    run_case(binary, 2, 1, mode)
    run_case(binary, 4, 1, mode)
    run_case(binary, 2, 2, mode)
    if mode == "--multi":
        run_unsynchronized_case(binary)
    print("ub-switch lifetime adapter synchronization smoke test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
