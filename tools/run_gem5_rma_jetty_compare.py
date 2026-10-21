#!/usr/bin/env python3
"""Compare one-way small-message READ/WRITE scaling with one UB port."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import re
import subprocess
import time

from run_modular_rma_regression import collect_fence_deltas, fence_logs, validate_fences


def command(op: str, size: int, jettys: int, iterations: int, port: int,
            server: str | None, depth: int, jfc_depth: int, post_list: int) -> str:
    parts = [
        "UBSIM_DIST_SYNC=1", "OPENURMA_DIST_SYNC=1",
        "LD_LIBRARY_PATH=/lib:/usr/lib", "urma_perftest", f"{op}_bw",
        "-d", "udma0", "--eid_idx", "0", "--ctp",
        "-s", str(size), "-P", str(port), "-J", str(jettys),
        "--jettys_pre_jfr", "1", "-I", "0", "-T", str(depth),
        "-R", str(depth), "-C", str(jfc_depth), "-n", str(iterations),
        "-l", str(post_list), "-Q", "1", "-p", "0",
    ]
    if server is not None:
        parts.extend(["-S", server])
    return " ".join(parts)


def parse_result(output: str, size: int, jettys: int,
                 total_ops: int) -> tuple[float, float, float]:
    counts = re.findall(r"Number of jettys\s*:\s*(\d+)", output)
    exits = re.findall(r"__UBSIM_RC_\d+_\d+__=(\d+)", output)
    rows = re.findall(
        rf"^\s*{size}\s+{total_ops}\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s*$",
        output, re.MULTILINE,
    )
    if counts != [str(jettys)] * 2 or exits != ["0"] * 2 or len(rows) != 2 or rows[0] != rows[1]:
        raise ValueError(f"invalid result: jettys={counts}, exits={exits}, rows={rows}")
    return tuple(map(float, rows[0]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path, required=True)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size", type=int, default=128)
    parser.add_argument("--jettys", type=int, nargs="+", default=[1, 2, 4, 8, 16])
    parser.add_argument("--total-ops", type=int, default=16384)
    parser.add_argument("--depth", type=int, default=8192)
    parser.add_argument("--jfc-depth", type=int, default=524288)
    parser.add_argument("--post-list", type=int, default=256)
    parser.add_argument("--repeat-one", action="store_true")
    parser.add_argument("--timeout", type=float, default=900)
    args = parser.parse_args()
    if args.size < 1 or args.post_list < 1 or args.depth < args.post_list or args.depth > 8192:
        parser.error("invalid size, post-list, or queue depth")
    if any(j < 1 or args.total_ops % j or args.total_ops // j < args.post_list
           for j in args.jettys):
        parser.error("each Jetty count must divide total ops and leave >=post-list ops per Jetty")
    if args.jfc_depth < max(j * min(args.depth, args.total_ops // j) for j in args.jettys):
        parser.error("JFC depth cannot cover the maximum aggregate Jetty window")
    manifest = (args.run_root / "run-manifest.txt").read_text()
    for required in ("profile=modern\n", "sync_request=roi\n", "ub_port_count=1\n",
                     "peer_link_rate_gbps=400\n"):
        if required not in manifest:
            parser.error(f"run manifest lacks {required.strip()}")
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "run-manifest.txt").write_text(manifest)
    cases = [(op, j, False) for j in args.jettys for op in ("read", "write")]
    if args.repeat_one:
        cases.extend((op, 1, True) for op in ("read", "write"))
    fields = ["sequence", "operation", "jettys", "repeat", "size_bytes",
              "iterations_per_jetty", "total_operations", "queue_depth",
              "jfc_depth", "post_list", "peak_mib_s", "average_mib_s",
              "message_rate_mpps", "wall_seconds", "transcript"]
    with (args.output / "results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for index, (op, jettys, repeat) in enumerate(cases):
            iterations = args.total_ops // jettys
            port = 21600 + index
            commands = (
                command(op, args.size, jettys, iterations, port, None,
                        args.depth, args.jfc_depth, args.post_list),
                command(op, args.size, jettys, iterations, port, "10.0.0.1",
                        args.depth, args.jfc_depth, args.post_list),
            )
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
            prefix = f"{index:02d}-{op}-s{args.size}-j{jettys}{'-repeat' if repeat else ''}"
            transcript = f"{prefix}.uart.txt"
            (args.output / transcript).write_text(result.stdout)
            logs, log_errors = collect_fence_deltas(paths, offsets, args.output, prefix)
            errors = [*log_errors, *validate_fences(logs)]
            try:
                peak, average, mpps = parse_result(result.stdout, args.size,
                                                   jettys, args.total_ops)
            except ValueError as error:
                errors.append(str(error))
            if result.returncode or errors:
                print(f"{op} J={jettys} failed: rc={result.returncode}, errors={errors}",
                      flush=True)
                return 1
            writer.writerow({"sequence": index + 1, "operation": op,
                             "jettys": jettys, "repeat": int(repeat),
                             "size_bytes": args.size,
                             "iterations_per_jetty": iterations,
                             "total_operations": args.total_ops,
                             "queue_depth": args.depth, "jfc_depth": args.jfc_depth,
                             "post_list": args.post_list,
                             "peak_mib_s": peak, "average_mib_s": average,
                             "message_rate_mpps": mpps,
                             "wall_seconds": f"{elapsed:.3f}",
                             "transcript": transcript})
            stream.flush()
            print(f"{op} J={jettys}{' repeat' if repeat else ''}: "
                  f"{average:.2f} MiB/s, {mpps:.3f} Mpps, wall {elapsed:.1f}s",
                  flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
