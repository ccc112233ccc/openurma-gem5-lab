#!/usr/bin/env python3
"""Run and time the official-perftest modular dataplane regression matrix."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import re
import subprocess
import time


FULL_CASES = [
    ("send_lat_128", "send_lat", 128, 20, 1, 1, False, None),
    ("send_bw_128_wrap", "send_bw", 128, 128, 16, 16, False, None),
    ("send_bw_4096", "send_bw", 4096, 32, 16, 16, False, None),
    ("write_lat_128", "write_lat", 128, 8, 1, 1, False, None),
    ("read_lat_128", "read_lat", 128, 8, 1, 1, False, None),
    ("write_bw_4096_out16", "write_bw", 4096, 64, 16, 16, True, None),
    ("read_bw_4096_out16", "read_bw", 4096, 64, 16, 16, True, None),
    ("write_bw_65536_frag", "write_bw", 65536, 16, 1, 1, True, None),
    ("read_bw_65536_frag", "read_bw", 65536, 16, 1, 1, True, None),
    ("write_bw_1m_frag", "write_bw", 1048576, 5, 1, 1, True, None),
    ("read_bw_1m_frag", "read_bw", 1048576, 5, 1, 1, True, None),
    # Keep all sizes in one process so SQ state crosses the 1- to 2-WQEBB
    # inline boundary and repeatedly wraps the ring.
    ("write_bw_all_2_to_1m_inline64", "write_bw", 1048576, 16, 1, 1,
     True, 20),
]

SYNC_SMOKE_CASES = [
    ("send_lat_128_sync", "send_lat", 128, 8, 1, 1, False, None),
    ("write_lat_128_sync", "write_lat", 128, 8, 1, 1, False, None),
    ("read_lat_128_sync", "read_lat", 128, 8, 1, 1, False, None),
    ("send_bw_128_sync", "send_bw", 128, 16, 16, 16, False, None),
]

GEM5_TICK_RE = re.compile(r"(?m)^(\d+):")


def last_gem5_tick(path: Path) -> int | None:
    """Return the newest timestamped gem5 log event, in one-picosecond ticks."""
    try:
        matches = GEM5_TICK_RE.findall(path.read_text(errors="replace"))
    except FileNotFoundError:
        return None
    return int(matches[-1]) if matches else None


def tick_delta(before: int | None, after: int | None) -> int | None:
    if before is None or after is None or after < before:
        return None
    return after - before


def command(verb: str, size: int, iterations: int, post_list: int,
            cq_mod: int, port: int, server: str | None,
            bidirectional: bool, dist_sync: bool,
            all_exponent: int | None) -> str:
    parts = [
        "LD_LIBRARY_PATH=/lib:/usr/lib", "urma_perftest", verb,
        "-d", "udma0", "--eid_idx", "0", "--ctp",
        "-P", str(port), "-J", "1", "-I", "64",
        "-n", str(iterations), "-l", str(post_list), "-Q", str(cq_mod),
        "-p", "0",
    ]
    parts.extend(
        [f"-a{all_exponent}"] if all_exponent is not None
        else ["-s", str(size)]
    )
    if dist_sync:
        parts.insert(0, "UBSIM_DIST_SYNC=1")
    if bidirectional:
        parts.append("-B")
    if server is not None:
        parts.extend(["-S", server])
    return " ".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lab", type=Path, required=True)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--uart0", type=int, default=3460)
    parser.add_argument("--uart1", type=int, default=3470)
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument(
        "--simulator", choices=("gem5", "qemu"), default="gem5",
        help="guest simulator; QEMU functional runs do not expose gem5 tick timing",
    )
    parser.add_argument(
        "--suite", choices=("full", "sync-smoke"), default="full",
        help="full functional stress without a fine-grained fence, or a small synchronized ROI smoke suite",
    )
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "model-manifest.txt").write_text(
        (args.run_root / "run-manifest.txt").read_text()
    )
    suite_started = time.perf_counter()
    records = []
    dist_sync = args.suite == "sync-smoke"
    cases = SYNC_SMOKE_CASES if dist_sync else FULL_CASES
    for index, (label, verb, size, iterations, post_list, cq_mod,
                bidirectional, all_exponent) in enumerate(cases):
        port = 21300 + index
        server_command = command(verb, size, iterations, post_list, cq_mod,
                                 port, None, bidirectional, dist_sync,
                                 all_exponent)
        client_command = command(verb, size, iterations, post_list, cq_mod,
                                 port, "10.0.0.1", bidirectional, dist_sync,
                                 all_exponent)
        raw = args.output / f"{index:02d}-{label}.uart.txt"
        ticks_before = (
            [
                last_gem5_tick(args.run_root / f"node{node}" / "gem5.log")
                for node in range(2)
            ]
            if args.simulator == "gem5" else [None, None]
        )
        started = time.perf_counter()
        result = subprocess.run(
            ["python3", str(args.lab / "tools/dual_serial_command.py"),
             "--ports", str(args.uart0), str(args.uart1), "--commands",
             server_command, client_command, "--timeout", str(args.timeout),
             "--prompt-kick-after", "1", "--stagger", "1",
             "--full-output"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        elapsed = time.perf_counter() - started
        ticks_after = (
            [
                last_gem5_tick(args.run_root / f"node{node}" / "gem5.log")
                for node in range(2)
            ]
            if args.simulator == "gem5" else [None, None]
        )
        tick_deltas = [
            tick_delta(before, after)
            for before, after in zip(ticks_before, ticks_after)
        ]
        valid_tick_deltas = [value for value in tick_deltas if value is not None]
        raw.write_text(result.stdout)
        record = {
            "case": label, "verb": verb, "size_bytes": size,
            "iterations": iterations, "post_list": post_list,
            "cq_mod": cq_mod, "bidirectional": bidirectional,
            "all_exponent": all_exponent,
            "wall_seconds": elapsed, "returncode": result.returncode,
            "node0_sim_ticks": tick_deltas[0],
            "node1_sim_ticks": tick_deltas[1],
            "simulated_elapsed_ns_max": (
                max(valid_tick_deltas) / 1000 if valid_tick_deltas else None
            ),
            "server_command": server_command,
            "client_command": client_command,
            "raw_output": raw.name,
        }
        records.append(record)
        print(f"[{label}] rc={result.returncode} wall={elapsed:.3f}s", flush=True)
    suite_wall = time.perf_counter() - suite_started
    report = {
        "simulator": args.simulator,
        "suite": args.suite,
        "virtual_time_synchronized": dist_sync,
        "simulated_time_source": (
            "max gem5 UART-boundary tick delta"
            if args.simulator == "gem5" else None
        ),
        "gem5_tick_period_ps": 1 if args.simulator == "gem5" else None,
        "suite_wall_seconds": suite_wall,
        "cases": records,
    }
    (args.output / "report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n"
    )
    with (args.output / "results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=[
            "case", "verb", "size_bytes", "iterations", "post_list",
            "cq_mod", "bidirectional", "all_exponent", "wall_seconds", "returncode",
            "node0_sim_ticks", "node1_sim_ticks",
            "simulated_elapsed_ns_max", "raw_output",
        ], extrasaction="ignore")
        writer.writeheader()
        writer.writerows(records)
    print(f"suite_wall_seconds={suite_wall:.3f}")
    return 0 if all(record["returncode"] == 0 for record in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
