#!/usr/bin/env python3
"""Measure selected synchronized, one-Jetty READ latencies after a ready restore."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import re
import shlex
import subprocess
import time

from run_modular_rma_regression import collect_fence_deltas, fence_logs, validate_fences


def command(size: int, iterations: int, port: int, server: str | None) -> str:
    parts = [
        "UBSIM_DIST_SYNC=1", "OPENURMA_DIST_SYNC=1",
        "LD_LIBRARY_PATH=/lib:/usr/lib", "urma_perftest", "read_lat",
        "-d", "udma0", "--eid_idx", "0", "--ctp",
        "-s", str(size), "-P", str(port), "-J", "1", "-I", "128",
        "-T", "8192", "-R", "8192", "-C", "524288",
        "-n", str(iterations), "-Q", "1", "-p", "0",
    ]
    if server is not None:
        parts.extend(["-S", server])
    return " ".join(parts)


def parse_result(output: str, size: int) -> dict[str, str]:
    number = r"[0-9]+(?:\.[0-9]+)?"
    matches = re.findall(
        rf"(?m)^\s*{size}\s+(\d+)\s+({number})\s+({number})\s+"
        rf"({number})\s+({number})\s+({number})\s+({number})\s+"
        rf"({number})\s+({number})\s+({number})\s*$", output,
    )
    exits = re.findall(r"__UBSIM_RC_\d+_\d+__=(\d+)", output)
    jettys = re.findall(r"Number of jettys\s*:\s*(\d+)", output)
    if len(matches) != 1 or exits != ["0", "0"] or jettys != ["1", "1"]:
        raise ValueError(f"invalid READ latency result: rows={matches}, exits={exits}, jettys={jettys}")
    fields = ("samples", "minimum_us", "maximum_us", "median_us", "average_us",
              "stdev_us", "p99_us", "p999_us", "p9999_us", "p99999_us")
    return dict(zip(fields, matches[0]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path, required=True)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sizes", type=int, nargs="+", default=[2, 128, 1024, 4096, 2])
    parser.add_argument("--iterations", type=int, default=4096)
    parser.add_argument("--client-library-path")
    parser.add_argument("--timeout", type=float, default=900)
    args = parser.parse_args()
    manifest = (args.run_root / "run-manifest.txt").read_text()
    for required in ("profile=modern\n", "sync_request=roi\n", "ub_port_count=1\n"):
        if required not in manifest:
            parser.error(f"run manifest lacks {required.strip()}")
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "run-manifest.txt").write_text(manifest)
    paths = fence_logs(args.run_root)
    fields = ("sequence", "size_bytes", "requested_iterations", "wall_seconds",
              "samples", "minimum_us", "maximum_us", "median_us", "average_us",
              "stdev_us", "p99_us", "p999_us", "p9999_us", "p99999_us", "transcript")
    with (args.output / "results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for index, size in enumerate(args.sizes):
            port = 21800 + index
            commands = (command(size, args.iterations, port, None),
                        command(size, args.iterations, port, "10.0.0.1"))
            if args.client_library_path:
                commands = (commands[0], commands[1].replace(
                    "LD_LIBRARY_PATH=/lib:/usr/lib",
                    "LD_LIBRARY_PATH=" + shlex.quote(args.client_library_path), 1))
            offsets = {name: path.stat().st_size if path.is_file() else None
                       for name, path in paths.items()}
            started = time.perf_counter()
            result = subprocess.run(
                ["python3", str(args.lab / "tools/dual_serial_command.py"),
                 "--ports", "3460", "3470", "--commands", *commands,
                 "--timeout", str(args.timeout), "--prompt-kick-after", "1",
                 "--stagger", "1", "--full-output"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            elapsed = time.perf_counter() - started
            transcript = f"read-lat-{index:02d}-s{size}.uart.txt"
            (args.output / transcript).write_text(result.stdout)
            logs, errors = collect_fence_deltas(paths, offsets, args.output,
                                                f"read-lat-{index:02d}-s{size}.fences")
            errors.extend(validate_fences(logs))
            if result.returncode or errors:
                print(f"size={size}: serial rc={result.returncode}, fence errors={errors}", flush=True)
                return 1
            try:
                parsed = parse_result(result.stdout, size)
            except ValueError as error:
                print(f"size={size}: {error}", flush=True)
                return 1
            writer.writerow({"sequence": index + 1, "size_bytes": size,
                             "requested_iterations": args.iterations,
                             "wall_seconds": f"{elapsed:.3f}", "transcript": transcript,
                             **parsed})
            stream.flush()
            print(f"size={size}: mean {parsed['average_us']} us, p99 {parsed['p99_us']} us, "
                  f"wall {elapsed:.1f}s", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
