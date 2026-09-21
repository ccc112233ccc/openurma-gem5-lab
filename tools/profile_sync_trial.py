#!/usr/bin/env python3
"""Profile one synchronized dual-node latency run without restarting guests."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time


PROCESS_FILES = {
    "node0": "node0/gem5.pid",
    "node1": "node1/gem5.pid",
    "dist_switch": "switch/gem5.pid",
    "ub_switch": "ub-switch/gem5.pid",
}


def docker_text(container: str, *command: str, check: bool = True) -> str:
    result = subprocess.run(
        ["docker", "exec", container, *command],
        check=check,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout


def process_snapshot(container: str, run_root: str) -> dict[str, dict[str, int]]:
    snapshots: dict[str, dict[str, int]] = {}
    for label, relative_pidfile in PROCESS_FILES.items():
        pidfile = f"{run_root}/{relative_pidfile}"
        pid_text = docker_text(container, "sh", "-c", f"cat '{pidfile}' 2>/dev/null || true")
        if not pid_text.strip().isdigit():
            continue
        pid = int(pid_text.strip())
        payload = docker_text(
            container,
            "python3",
            "-c",
            "import json,pathlib,sys;"
            "p=pathlib.Path('/proc')/sys.argv[1];"
            "s=(p/'stat').read_text();r=s[s.rfind(')')+2:].split();"
            "st=(p/'status').read_text().splitlines();"
            "kv={x.split(':',1)[0]:x.split(':',1)[1].strip() for x in st if ':' in x};"
            "io={x.split(':',1)[0]:int(x.split(':',1)[1]) for x in (p/'io').read_text().splitlines()};"
            "print(json.dumps({'pid':int(sys.argv[1]),'minflt':int(r[7]),'majflt':int(r[9]),"
            "'utime_ticks':int(r[11]),'stime_ticks':int(r[12]),'num_threads':int(r[17]),"
            "'voluntary_ctxt_switches':int(kv.get('voluntary_ctxt_switches','0')),'nonvoluntary_ctxt_switches':int(kv.get('nonvoluntary_ctxt_switches','0')),'read_bytes':io.get('read_bytes',0),'write_bytes':io.get('write_bytes',0)}))",
            str(pid),
            check=False,
        )
        if payload.strip():
            snapshots[label] = json.loads(payload)
    return snapshots


def subtract(before: dict[str, dict[str, int]], after: dict[str, dict[str, int]],
             ticks_per_second: int) -> dict[str, dict[str, float | int]]:
    result: dict[str, dict[str, float | int]] = {}
    for label in sorted(before.keys() & after.keys()):
        if before[label]["pid"] != after[label]["pid"]:
            continue
        delta = {
            key: after[label][key] - before[label][key]
            for key in before[label]
            if key not in ("pid", "num_threads")
        }
        delta["cpu_seconds"] = (
            delta["utime_ticks"] + delta["stime_ticks"]
        ) / ticks_per_second
        delta["pid"] = before[label]["pid"]
        result[label] = delta
    return result


def stats_offsets(run_root: Path) -> dict[str, int]:
    return {
        node: (run_root / node / "stats.txt").stat().st_size
        if (run_root / node / "stats.txt").exists() else 0
        for node in ("node0", "node1")
    }


def parse_new_stats(run_root: Path, offsets: dict[str, int]) -> dict[str, dict[str, float | int]]:
    fields = ("simTicks", "simInsts", "hostSeconds", "hostTickRate")
    result: dict[str, dict[str, float | int]] = {}
    for node, offset in offsets.items():
        path = run_root / node / "stats.txt"
        if not path.exists():
            continue
        with path.open("rb") as stream:
            stream.seek(offset)
            text = stream.read().decode(errors="replace")
        values: dict[str, float | int] = {}
        for field in fields:
            matches = re.findall(rf"^{field}\s+([0-9.eE+-]+)", text, re.MULTILINE)
            if matches:
                value = float(matches[-1])
                values[field] = int(value) if value.is_integer() else value
        result[node] = values
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--label", required=True)
    parser.add_argument("--samples", type=int, required=True)
    parser.add_argument("--size", type=int, required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--container", default="openurma-gem5-lab")
    parser.add_argument("--run-root", type=Path, default=Path("run-dual"))
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    raw_path = args.output_dir / f"{args.label}.uart.txt"
    json_path = args.output_dir / f"{args.label}.json"
    container_run_root = os.environ.get(
        "OPENURMA_DUAL_OUT", "/workspace/openurma-gem5-lab/run-dual"
    )
    ticks_per_second = int(docker_text(
        args.container, "python3", "-c",
        "import os; print(os.sysconf('SC_CLK_TCK'))"
    ).strip())
    before = process_snapshot(args.container, container_run_root)
    offsets = stats_offsets(args.run_root)

    command = [
        "bash", "run-latency.sh", "--samples", str(args.samples),
        "--size", str(args.size), "--port", str(args.port), "--roi-stats",
        "--format", "tsv", "--raw-output", str(raw_path), "--timeout", "600",
    ]
    started = time.perf_counter()
    completed = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    wall_seconds = time.perf_counter() - started
    after = process_snapshot(args.container, container_run_root)

    report = {
        "label": args.label,
        "samples": args.samples,
        "size_bytes": args.size,
        "port": args.port,
        "wall_seconds": wall_seconds,
        "returncode": completed.returncode,
        "process_deltas": subtract(before, after, ticks_per_second),
        "gem5_roi_stats": parse_new_stats(args.run_root, offsets),
        "result_tsv": completed.stdout,
        "stderr": completed.stderr,
    }
    json_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps(report, indent=2, sort_keys=True))
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
