#!/usr/bin/env python3
"""Capture matched two-host READ size scans with original UART evidence."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import subprocess
import time

from run_gem5_read_latency_sweep import parse_result as parse_latency
from run_gem5_read_size_sweep import parse_result as parse_bandwidth
from run_modular_rma_regression import collect_fence_deltas, fence_logs, validate_fences


def command(simulator: str, kind: str, size: int, count: int,
            port: int, client: bool) -> str:
    prefix = ("UBSIM_DIST_SYNC=1 OPENURMA_DIST_SYNC=1 "
              if simulator == "gem5" else "")
    flags = (f"-d udma0 --eid_idx 0 --ctp -s {size} -P {port} -J 1 "
             f"-T 8192 -R 8192 -C 524288 -n {count} -Q 1 -p 0")
    if kind == "bw":
        flags += " -I 0 -l 256"
    else:
        flags += " -I 128"
    return (f"{prefix}LD_LIBRARY_PATH=/lib:/usr/lib urma_perftest read_{kind} "
            f"{flags}" + (" -S 10.0.0.1" if client else ""))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--simulator", choices=("gem5", "qemu"), required=True)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sizes", type=int, nargs="+", required=True)
    parser.add_argument("--bw-iterations", type=int, default=4096)
    parser.add_argument("--lat-iterations", type=int, default=256)
    parser.add_argument("--bw-target-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--timeout", type=int, default=450)
    parser.add_argument("--allow-unsynchronized", action="store_true",
                        help="accept a QEMU run with all SimBricks synchronization disabled")
    args = parser.parse_args()
    if any(size < 1 for size in args.sizes):
        parser.error("sizes must be positive")
    manifest = (args.run_root / "run-manifest.txt").read_text()
    sync_marker = ("sync_request=roi\n" if args.simulator == "gem5" else
                   "synchronization=off\n" if args.allow_unsynchronized else
                   "synchronization=required\n")
    if sync_marker not in manifest or "node_count=2\n" not in manifest:
        parser.error("expected a synchronized two-host run")
    if args.simulator == "gem5" and not (args.run_root / "sync.ready").is_file():
        parser.error("run ./lab sync before measuring gem5")
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "run-manifest.txt").write_text(manifest)
    uart = (3460, 3470) if args.simulator == "gem5" else (3560, 3570)
    fences = fence_logs(args.run_root) if args.simulator == "gem5" else {}
    for kind in ("lat", "bw"):
        fields = (["size_bytes", "iterations", "samples", "average_us", "p99_us", "wall_seconds", "transcript"]
                  if kind == "lat" else
                  ["size_bytes", "iterations", "peak_mib_s", "average_mib_s", "message_rate_mpps", "wall_seconds", "transcript"])
        with (args.output / f"read-{kind}.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            for index, size in enumerate(args.sizes):
                count = (args.lat_iterations if kind == "lat" else
                         max(256, min(args.bw_iterations,
                                      args.bw_target_bytes // size // 256 * 256)))
                port = (22100 if kind == "lat" else 22200) + index
                server = command(args.simulator, kind, size, count, port, False)
                client = command(args.simulator, kind, size, count, port, True)
                offsets = {name: path.stat().st_size if path.is_file() else None
                           for name, path in fences.items()}
                start = time.perf_counter()
                result = subprocess.run(
                    ["python3", str(Path(__file__).with_name("dual_serial_command.py")),
                     "--ports", *map(str, uart), "--commands", server, client,
                     "--stagger", "1", "--prompt-kick-after", "1",
                     "--timeout", str(args.timeout), "--full-output"],
                    capture_output=True, text=True, check=False)
                elapsed = time.perf_counter() - start
                transcript = f"read-{kind}-{index:02d}-s{size}.uart.txt"
                (args.output / transcript).write_text(result.stdout + result.stderr)
                if fences:
                    logs, errors = collect_fence_deltas(
                        fences, offsets, args.output, f"read-{kind}-{index:02d}-s{size}.fences")
                    errors.extend(validate_fences(logs))
                    if errors:
                        raise RuntimeError(f"{transcript}: synchronization errors: {errors}")
                if result.returncode:
                    raise RuntimeError(f"{transcript}: command failed with {result.returncode}")
                if kind == "lat":
                    parsed = parse_latency(result.stdout, size)
                    row = {"size_bytes": size, "iterations": count,
                           "samples": parsed["samples"],
                           "average_us": parsed["average_us"],
                           "p99_us": parsed["p99_us"],
                           "wall_seconds": f"{elapsed:.3f}", "transcript": transcript}
                    value = f"{parsed['average_us']} us"
                else:
                    peak, average, rate = parse_bandwidth(result.stdout, size, count)
                    row = {"size_bytes": size, "iterations": count,
                           "peak_mib_s": peak, "average_mib_s": average,
                           "message_rate_mpps": rate,
                           "wall_seconds": f"{elapsed:.3f}", "transcript": transcript}
                    value = f"{average} MiB/s"
                writer.writerow(row)
                stream.flush()
                print(f"{args.simulator} READ {kind} {size}B: {value}, wall {elapsed:.1f}s", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
