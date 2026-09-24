"""Unified CLI for building, launching, and operating the simulation lab.

The shell scripts below ``scripts/`` are implementation backends.  This module
is the stable user interface and owns runtime selection, node addressing, and
command discovery.
"""

from __future__ import annotations

import os
import platform
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


ROOT = Path(__file__).resolve().parent.parent


@dataclass(frozen=True)
class Command:
    script: str
    summary: str


COMMANDS = {
    "start": Command("scripts/run/run-dual.sh", "start the multi-node full-system simulation"),
    "start-single": Command("scripts/run/run.sh", "start the legacy single-node simulation"),
    "status": Command("scripts/run/status-dual.sh", "show node and switch process state"),
    "stop": Command("scripts/run/stop-dual.sh", "stop the current multi-node simulation"),
    "sync": Command("scripts/run/sync-dual.sh", "finish guest network and time-sync setup"),
    "latency": Command("scripts/run/run-latency.sh", "run the standard two-node send_lat test"),
    "latency-pair": Command("scripts/run/run-node-pair-latency.sh", "test an arbitrary server/client node pair"),
    "latency-pairs": Command("scripts/run/run-paired-latency.sh", "test adjacent node pairs concurrently"),
    "sweep-latency": Command("scripts/run/sweep-latency.sh", "scan send_lat across message sizes"),
    "validate-server": Command("scripts/validation/validate-server-profile.sh", "validate the modeled server profile"),
}

BUILD_TARGETS = {
    "all": "scripts/build-all.sh",
    "gem5": "scripts/build/build_gem5.sh",
    "kernel": "scripts/build/build_olk66.sh",
    "umdk": "scripts/build/build_umdk.sh",
    "initramfs": "scripts/build/build-interactive-initramfs.sh",
    "ns3ub": "scripts/build-ns3ub-adapter.sh",
    "mooncake": "scripts/build-mooncake-urma.sh",
    "mooncake-initramfs": "scripts/package-mooncake-urma-initramfs.sh",
}


def _usage(stream=None) -> None:
    if stream is None:
        stream = sys.stdout
    print(
        "usage: ./lab [--runtime auto|docker|native] COMMAND [ARGS...]\n\n"
        "Lifecycle:\n"
        "  setup             fetch and build the reproducible environment\n"
        "  start             start N gem5 nodes plus the UB/OOB switches\n"
        "  status            show simulator process and guest readiness\n"
        "  sync              initialize the guest control network\n"
        "  attach NODE       connect to node NODE's PL011 console\n"
        "  stop              stop the current simulation\n\n"
        "Experiments:\n"
        "  latency           run the standard two-node send_lat workload\n"
        "  latency-pair      run send_lat between arbitrary node IDs\n"
        "  latency-pairs     run adjacent pairs concurrently\n"
        "  sweep-latency     scan latency across message sizes\n\n"
        "Build and validation:\n"
        "  build TARGET      TARGET: " + ", ".join(BUILD_TARGETS) + "\n"
        "  validate-server   validate the instantiated server profile\n"
        "  start-single      start the legacy single-node environment\n\n"
        "Use './lab COMMAND --help' for backend-specific options. Runtime\n"
        "defaults to native on ARM64 Linux and Docker elsewhere.",
        file=stream,
    )


def _runtime(value: str) -> str:
    if value == "auto":
        if platform.system() == "Linux" and platform.machine() in {"aarch64", "arm64"}:
            return "native"
        return "docker"
    if value not in {"docker", "native"}:
        raise ValueError("runtime must be auto, docker, or native")
    return value


def _split_global_options(argv: Sequence[str]) -> tuple[str, list[str]]:
    runtime = os.environ.get("OPENURMA_EXECUTION_MODE", "auto")
    args = list(argv)
    while args and args[0].startswith("--"):
        option = args.pop(0)
        if option == "--help":
            _usage()
            raise SystemExit(0)
        if option == "--version":
            from . import __version__

            print(__version__)
            raise SystemExit(0)
        if option == "--runtime":
            if not args:
                raise ValueError("--runtime requires a value")
            runtime = args.pop(0)
            continue
        if option.startswith("--runtime="):
            runtime = option.split("=", 1)[1]
            continue
        raise ValueError(f"unknown global option: {option}")
    return _runtime(runtime), args


def _script(relative: str, args: Sequence[str], runtime: str) -> int:
    path = ROOT / relative
    if not path.is_file():
        print(f"lab: internal command is missing: {path}", file=sys.stderr)
        return 2
    env = os.environ.copy()
    env["OPENURMA_EXECUTION_MODE"] = runtime
    env.setdefault("OPENURMA_LAB_ROOT", str(ROOT))
    return subprocess.run(["bash", str(path), *args], env=env).returncode


def _requested_target_arch(args: Sequence[str]) -> str | None:
    for index, value in enumerate(args):
        if value == "--target-arch" and index + 1 < len(args):
            return args[index + 1]
        if value.startswith("--target-arch="):
            return value.split("=", 1)[1]
    return None


def _attach(args: Sequence[str], runtime: str) -> int:
    if len(args) != 1 or not args[0].isdigit():
        print("usage: ./lab [--runtime ...] attach NODE", file=sys.stderr)
        return 2
    node = int(args[0])
    uart0 = int(os.environ.get("OPENURMA_DUAL_UART0", "3460"))
    uart1 = int(os.environ.get("OPENURMA_DUAL_UART1", "3470"))
    stride = uart1 - uart0
    if node < 0 or stride <= 0:
        print("lab: NODE must be non-negative and node1 UART must exceed node0 UART", file=sys.stderr)
        return 2
    env = os.environ.copy()
    env.setdefault("OPENURMA_M5TERM_PORT", str(uart0 + node * stride))
    env["OPENURMA_EXECUTION_MODE"] = runtime
    env.setdefault("OPENURMA_LAB_ROOT", str(ROOT))
    return subprocess.run(["bash", str(ROOT / "scripts/run/attach.sh")], env=env).returncode


def main(argv: Sequence[str] | None = None) -> int:
    try:
        runtime, args = _split_global_options(sys.argv[1:] if argv is None else argv)
    except ValueError as exc:
        print(f"lab: {exc}", file=sys.stderr)
        _usage(sys.stderr)
        return 2

    if not args or args[0] in {"help", "-h"}:
        _usage()
        return 0
    command, tail = args[0], args[1:]

    if command == "setup":
        target_arch = _requested_target_arch(tail)
        if target_arch in {"x86_64", "amd64"} and runtime != "native":
            print(
                "lab: x86_64 UMDK setup requires '--runtime native' on x86_64 Linux",
                file=sys.stderr,
            )
            return 2
        script = "scripts/setup-native.sh" if runtime == "native" else "scripts/setup-docker.sh"
        return _script(script, tail, runtime)
    if command == "attach":
        return _attach(tail, runtime)
    if command == "build":
        if not tail or tail[0] not in BUILD_TARGETS:
            print("lab: build target must be one of: " + ", ".join(BUILD_TARGETS), file=sys.stderr)
            return 2
        target_arch = _requested_target_arch(tail[1:])
        if target_arch in {"x86_64", "amd64"} and tail[0] != "umdk":
            print(
                "lab: x86_64 currently supports the UMDK userspace target only; "
                "the official UB/UMMU kernel and gem5 machine are ARM64-only",
                file=sys.stderr,
            )
            return 2
        return _script(BUILD_TARGETS[tail[0]], tail[1:], runtime)
    if command in COMMANDS:
        return _script(COMMANDS[command].script, tail, runtime)

    print(f"lab: unknown command: {command}", file=sys.stderr)
    _usage(sys.stderr)
    return 2
