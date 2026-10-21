#!/usr/bin/env python3
"""Measure one-way 512-B CTP WRITE bandwidth as Jetty count grows."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import re
import subprocess
import time

from run_modular_rma_regression import collect_fence_deltas, fence_logs, validate_fences


def command(jettys: int, iterations: int, port: int, server: str | None,
            queue_depth: int, jfc_depth: int, post_list: int) -> str:
    parts = [
        "UBSIM_DIST_SYNC=1", "OPENURMA_DIST_SYNC=1",
        "LD_LIBRARY_PATH=/lib:/usr/lib", "urma_perftest", "write_bw",
        "-d", "udma0", "--eid_idx", "0", "--ctp",
        "-s", "512", "-P", str(port), "-J", str(jettys),
        "--jettys_pre_jfr", "1", "-I", "0",
        "-T", str(queue_depth), "-R", str(queue_depth),
        "-C", str(jfc_depth), "-n", str(iterations),
        "-l", str(post_list), "-Q", "1",
        "-p", "0",
    ]
    if server is not None:
        parts.extend(["-S", server])
    return " ".join(parts)


def parse_result(output: str, jettys: int, total_ops: int) -> tuple[float, float, float]:
    counts = re.findall(r"Number of jettys\s*:\s*(\d+)", output)
    if counts != [str(jettys), str(jettys)]:
        raise ValueError(f"Jetty headers differ: {counts}")
    exits = re.findall(r"__UBSIM_RC_\d+_\d+__=(\d+)", output)
    if exits != ["0", "0"]:
        raise ValueError(f"guest exit codes differ: {exits}")
    rows = re.findall(
        rf"^\s*512\s+{total_ops}\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s*$",
        output, re.MULTILINE,
    )
    if len(rows) != 2 or rows[0] != rows[1]:
        raise ValueError(f"expected two matching WRITE rows, got {rows}")
    peak, average, mpps = map(float, rows[0])
    return peak, average, mpps


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lab", type=Path, required=True)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jettys", type=int, nargs="+", default=[1, 2, 4, 8, 16])
    parser.add_argument("--total-ops", type=int, default=16384)
    parser.add_argument("--queue-depth", type=int, default=512)
    parser.add_argument("--jfc-depth", type=int, default=32768)
    parser.add_argument("--post-list", type=int, default=256)
    parser.add_argument("--timeout", type=float, default=900)
    args = parser.parse_args()
    if any(count < 1 or args.total_ops % count or args.total_ops // count < args.post_list
           for count in args.jettys):
        parser.error("each Jetty count must divide total ops and leave >=post-list ops per Jetty")
    if args.queue_depth < args.post_list or args.queue_depth > 8192:
        parser.error("queue depth must be between post-list and 8192")
    if args.jfc_depth < max(count * min(args.queue_depth, args.total_ops // count)
                            for count in args.jettys):
        parser.error("JFC depth must cover the maximum total initial Jetty window")
    manifest = (args.run_root / "run-manifest.txt").read_text()
    if "profile=modern\n" not in manifest or "sync_request=roi\n" not in manifest:
        parser.error("requires a modern synchronized gem5 run")
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "run-manifest.txt").write_text(manifest)
    fields = ["jettys", "iterations_per_jetty", "total_operations",
              "queue_depth", "jfc_depth", "post_list", "peak_mib_s",
              "average_mib_s", "message_rate_mpps", "wall_seconds", "transcript"]
    with (args.output / "results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for index, jettys in enumerate(args.jettys):
            iterations = args.total_ops // jettys
            port = 21520 + index
            commands = (command(jettys, iterations, port, None,
                                args.queue_depth, args.jfc_depth, args.post_list),
                        command(jettys, iterations, port, "10.0.0.1",
                                args.queue_depth, args.jfc_depth, args.post_list))
            paths = fence_logs(args.run_root)
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
            prefix = f"write-bw-512-{index:02d}-j{jettys}"
            transcript = f"{prefix}.uart.txt"
            (args.output / transcript).write_text(result.stdout)
            logs, log_errors = collect_fence_deltas(paths, offsets, args.output,
                                                     prefix)
            errors = [*log_errors, *validate_fences(logs)]
            try:
                peak, average, mpps = parse_result(result.stdout, jettys, args.total_ops)
            except ValueError as error:
                errors.append(str(error))
            if result.returncode or errors:
                print(f"J={jettys} failed: rc={result.returncode}, errors={errors}", flush=True)
                return 1
            writer.writerow({"jettys": jettys, "iterations_per_jetty": iterations,
                             "total_operations": args.total_ops,
                             "queue_depth": args.queue_depth,
                             "jfc_depth": args.jfc_depth, "post_list": args.post_list,
                             "peak_mib_s": peak,
                             "average_mib_s": average, "message_rate_mpps": mpps,
                             "wall_seconds": f"{elapsed:.3f}", "transcript": transcript})
            stream.flush()
            print(f"J={jettys}: {average:.2f} MiB/s, {mpps:.3f} Mpps, wall {elapsed:.1f}s",
                  flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
