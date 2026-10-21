#!/usr/bin/env python3
"""Measure single-Jetty READ bandwidth at selected sizes in one gem5 restore."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import re
import shlex
import subprocess
import time


def command(size: int, iterations: int, port: int, server: str | None) -> str:
    parts = [
        "UBSIM_DIST_SYNC=1", "OPENURMA_DIST_SYNC=1",
        "LD_LIBRARY_PATH=/lib:/usr/lib", "urma_perftest", "read_bw",
        "-d", "udma0", "--eid_idx", "0", "--ctp",
        "-s", str(size), "-P", str(port), "-J", "1", "-I", "0",
        "-T", "8192", "-R", "8192", "-C", "524288",
        "-n", str(iterations), "-l", "256", "-Q", "1", "-p", "0",
    ]
    if server is not None:
        parts.extend(["-S", server])
    return " ".join(parts)


def parse_result(output: str, size: int, iterations: int) -> tuple[float, float, float]:
    counts = re.findall(r"Number of jettys\s*:\s*(\d+)", output)
    exits = re.findall(r"__UBSIM_RC_\d+_\d+__=(\d+)", output)
    rows = re.findall(
        rf"^\s*{size}\s+{iterations}\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s*$",
        output, re.MULTILINE,
    )
    if counts != ["1", "1"] or exits != ["0", "0"] or len(rows) != 2 or rows[0] != rows[1]:
        raise ValueError(f"invalid READ result: jettys={counts}, exits={exits}, rows={rows}")
    return tuple(map(float, rows[0]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path, required=True)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sizes", type=int, nargs="+", required=True)
    parser.add_argument("--target-bytes", type=int,
                        help="cap each size near this payload volume; -n stays a multiple of 256")
    parser.add_argument("--client-library-path")
    parser.add_argument("--timeout", type=float, default=900)
    args = parser.parse_args()
    if any(size < 1 for size in args.sizes):
        parser.error("sizes must be positive")
    if args.target_bytes is not None and args.target_bytes < 1:
        parser.error("target bytes must be positive")
    manifest = (args.run_root / "run-manifest.txt").read_text()
    for required in ("profile=modern\n", "sync_request=roi\n", "ub_port_count=1\n"):
        if required not in manifest:
            parser.error(f"run manifest lacks {required.strip()}")
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "run-manifest.txt").write_text(manifest)
    fields = ["sequence", "size_bytes", "iterations", "jettys", "peak_mib_s",
              "average_mib_s", "message_rate_mpps", "wall_seconds", "transcript"]
    with (args.output / "results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for index, size in enumerate(args.sizes):
            iterations = 16384
            if args.target_bytes is not None:
                iterations = max(256, min(16384, (args.target_bytes // size // 256) * 256))
            port = 21520 + index
            commands = (command(size, iterations, port, None),
                        command(size, iterations, port, "10.0.0.1"))
            if args.client_library_path:
                commands = (commands[0], commands[1].replace(
                    "LD_LIBRARY_PATH=/lib:/usr/lib",
                    "LD_LIBRARY_PATH=" + shlex.quote(args.client_library_path), 1))
            started = time.perf_counter()
            result = subprocess.run(
                ["python3", str(args.lab / "tools/dual_serial_command.py"),
                 "--ports", "3460", "3470", "--commands", *commands,
                 "--timeout", str(args.timeout), "--prompt-kick-after", "1",
                 "--stagger", "1", "--full-output"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            elapsed = time.perf_counter() - started
            transcript = f"read-bw-{index:02d}-s{size}.uart.txt"
            (args.output / transcript).write_text(result.stdout)
            if result.returncode:
                print(f"size={size}: serial command failed, rc={result.returncode}", flush=True)
                return 1
            try:
                peak, average, mpps = parse_result(result.stdout, size, iterations)
            except ValueError as error:
                print(f"size={size}: {error}", flush=True)
                return 1
            writer.writerow({"sequence": index + 1, "size_bytes": size,
                             "iterations": iterations, "jettys": 1,
                             "peak_mib_s": peak, "average_mib_s": average,
                             "message_rate_mpps": mpps,
                             "wall_seconds": f"{elapsed:.3f}", "transcript": transcript})
            stream.flush()
            print(f"size={size}, n={iterations}: {average:.2f} MiB/s, "
                  f"{mpps:.3f} Mpps, wall {elapsed:.1f}s",
                  flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
