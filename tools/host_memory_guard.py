#!/usr/bin/env python3
"""Check host memory before gem5 work and stop this run on sustained pressure."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import time

GIB = 1024 ** 3


def memory_bytes() -> tuple[int, int]:
    if sys.platform == "darwin":
        output = subprocess.check_output(["memory_pressure", "-Q"], text=True)
        total_match = re.search(r"The system has (\d+) ", output)
        free_match = re.search(r"System-wide memory free percentage: (\d+)%", output)
        if not total_match or not free_match:
            raise RuntimeError("cannot read macOS memory pressure")
        total = int(total_match.group(1))
        return total, total * int(free_match.group(1)) // 100
    if sys.platform.startswith("linux"):
        info = dict(re.findall(r"^(MemTotal|MemAvailable):\s+(\d+) kB$",
                               Path("/proc/meminfo").read_text(), re.MULTILINE))
        if len(info) != 2:
            raise RuntimeError("cannot read Linux MemAvailable")
        return int(info["MemTotal"]) * 1024, int(info["MemAvailable"]) * 1024
    raise RuntimeError(f"unsupported host platform: {sys.platform}")


def capacity_bytes(value: str) -> int:
    match = re.fullmatch(r"(\d+)(B|kB|KiB|MB|MiB|GB|GiB)", value)
    if not match:
        raise ValueError(f"invalid modeled memory size: {value}")
    scale = {"B": 1, "kB": 1000, "KiB": 1024,
             "MB": 1000 ** 2, "MiB": 1024 ** 2,
             "GB": 1000 ** 3, "GiB": GIB}
    return int(match.group(1)) * scale[match.group(2)]


def reserve_bytes(total: int) -> int:
    return max(2 * GIB, total // 5)


def describe(total: int, available: int) -> str:
    return (f"host memory: {available / GIB:.1f} GiB available / "
            f"{total / GIB:.1f} GiB total; reserve {reserve_bytes(total) / GIB:.1f} GiB")


def check(phase: str, nodes: int, mem_size: str) -> int:
    total, available = memory_bytes()
    print(describe(total, available), flush=True)
    launch_headroom = max(2 * GIB, nodes * 5 * GIB // 4)
    if phase == "qemu-start":
        launch_headroom = max(launch_headroom,
                              nodes * capacity_bytes(mem_size) + GIB)
    if available < reserve_bytes(total) + launch_headroom:
        print(f"memory guard: insufficient headroom for {nodes} guests "
              f"(need {launch_headroom / GIB:.1f} GiB above reserve)",
              file=sys.stderr)
        return 2
    if phase == "checkpoint" and nodes >= 3 and \
            nodes * capacity_bytes(mem_size) > 4 * total:
        print("memory guard: checkpointing this many large modeled memories "
              "exceeds the host budget; use a smaller --mem-size or a "
              "larger host", file=sys.stderr)
        return 2
    return 0


def live_guest(run_root: Path, nodes: int, pid_name: str) -> bool:
    for node in range(nodes):
        pidfile = run_root / f"node{node}" / pid_name
        try:
            pid = int(pidfile.read_text().strip())
            os.kill(pid, 0)
            return True
        except (OSError, ValueError):
            continue
    return False


def monitor(run_root: Path, nodes: int, stop_script: Path, interval: float,
            pid_name: str = "gem5.pid") -> int:
    manifest = run_root / "run-manifest.txt"
    initial = manifest.stat()
    identity = (initial.st_dev, initial.st_ino, initial.st_mtime_ns)
    logfile = run_root / "memory-guard.log"
    low_count = 0
    samples = 0
    with logfile.open("a", buffering=1) as log:
        while True:
            try:
                stat = manifest.stat()
                if (stat.st_dev, stat.st_ino, stat.st_mtime_ns) != identity or \
                        not live_guest(run_root, nodes, pid_name):
                    return 0
                total, available = memory_bytes()
                low_count = low_count + 1 if available < reserve_bytes(total) else 0
                if samples % 15 == 0 or low_count:
                    print(f"{time.strftime('%Y-%m-%dT%H:%M:%S%z')} "
                          f"{describe(total, available)} low_samples={low_count}",
                          file=log)
                if low_count >= 2:
                    reason = (f"memory guard stopped this run: available "
                              f"{available / GIB:.1f} GiB fell below reserve "
                              f"{reserve_bytes(total) / GIB:.1f} GiB")
                    print(reason, file=log)
                    (run_root / "memory-guard.stopped").write_text(reason + "\n")
                    subprocess.run(["bash", str(stop_script)],
                                   stdout=log, stderr=log, check=False)
                    return 2
                samples += 1
                time.sleep(interval)
            except FileNotFoundError:
                return 0
            except Exception as error:
                print(f"memory guard sampling failed: {error}", file=log)
                return 2


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="action", required=True)
    preflight = sub.add_parser("check")
    preflight.add_argument("--phase", choices=("start", "qemu-start", "checkpoint"),
                           required=True)
    preflight.add_argument("--nodes", type=int, required=True)
    preflight.add_argument("--mem-size", required=True)
    watch = sub.add_parser("monitor")
    watch.add_argument("--run-root", type=Path, required=True)
    watch.add_argument("--nodes", type=int, required=True)
    watch.add_argument("--stop-script", type=Path, required=True)
    watch.add_argument("--pid-name", choices=("gem5.pid", "qemu.pid"),
                       default="gem5.pid")
    watch.add_argument("--interval", type=float, default=2)
    args = parser.parse_args()
    try:
        if args.action == "check":
            return check(args.phase, args.nodes, args.mem_size)
        return monitor(args.run_root, args.nodes, args.stop_script,
                       args.interval, args.pid_name)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"memory guard: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
