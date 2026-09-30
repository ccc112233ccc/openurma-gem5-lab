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
    "start-qemu": Command("scripts/run/run-qemu.sh", "start the QEMU UB-HOST probe environment"),
    "start-qemu-dual": Command("scripts/run/run-qemu-dual.sh", "start two QEMU guests on the modular ns-3 UB fabric"),
    "status-qemu": Command("scripts/run/status-qemu-dual.sh", "show the QEMU dual-node process and boot state"),
    "stop-qemu": Command("scripts/run/stop-qemu-dual.sh", "stop the QEMU dual-node environment"),
    "status": Command("scripts/run/status-dual.sh", "show node and switch process state"),
    "stop": Command("scripts/run/stop-dual.sh", "stop the current multi-node simulation"),
    "sync": Command("scripts/run/sync-dual.sh", "finish guest network and time-sync setup"),
    "checkpoint": Command("scripts/run/checkpoint-dual.sh", "save a coordinated gem5/UDMA checkpoint"),
    "latency": Command("scripts/run/run-latency.sh", "run the standard two-node send_lat test"),
    "latency-pair": Command("scripts/run/run-node-pair-latency.sh", "test an arbitrary server/client node pair"),
    "latency-pairs": Command("scripts/run/run-paired-latency.sh", "test adjacent node pairs concurrently"),
    "sweep-latency": Command("scripts/run/sweep-latency.sh", "scan send_lat across message sizes"),
    "rma-regression": Command("scripts/run/run-rma-regression.sh", "run the timed modular SEND/READ/WRITE matrix"),
    "qemu-rma-regression": Command(
        "scripts/run/run-qemu-rma-regression.sh",
        "run the QEMU functional SEND/READ/WRITE matrix",
    ),
    "validate-server": Command("scripts/validation/validate-server-profile.sh", "validate the modeled server profile"),
}

BUILD_TARGETS = {
    "all": "scripts/build-all.sh",
    "gem5": "scripts/build/build_gem5.sh",
    "qemu": "scripts/build/build_qemu.sh",
    "kernel": "scripts/build/build_olk66.sh",
    "umdk": "scripts/build/build_umdk.sh",
    "initramfs": "official-udma/build_initramfs.sh",
    "ns3ub": "scripts/build-ns3ub-adapter.sh",
    "udma-model": "scripts/build/build_udma_model.sh",
    "udma-device": "scripts/build/build_udma_device_sim.sh",
    "ub-switch": "scripts/build/build_ub_switch_sim.sh",
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
        "  checkpoint NAME   save a shell-ready coordinated checkpoint\n"
        "  attach NODE       connect to node NODE's PL011 console\n"
        "  stop              stop the current simulation\n\n"
        "Experiments:\n"
        "  latency           run the standard two-node send_lat workload\n"
        "  latency-pair      run send_lat between arbitrary node IDs\n"
        "  latency-pairs     run adjacent pairs concurrently\n"
        "  sweep-latency     scan latency across message sizes\n\n"
        "  rma-regression    run timed SEND/READ/WRITE fragmentation and SQ tests\n\n"
        "  qemu-rma-regression  run the same functional matrix on dual QEMU\n\n"
        "Build and validation:\n"
        "  build TARGET      TARGET: " + ", ".join(BUILD_TARGETS) + "\n"
        "  validate-server   validate the instantiated server profile\n"
        "  start-qemu        start the interactive QEMU/UDMA probe environment\n\n"
        "  start-qemu-dual   start two QEMU guests plus UDMA and ns-3 UB fabric\n"
        "  attach-qemu NODE  connect to a QEMU guest serial console\n"
        "  status-qemu       show QEMU dual-node status\n"
        "  stop-qemu         stop the QEMU dual-node environment\n\n"
        "Use './lab COMMAND --help' for backend-specific options. Runtime\n"
        "defaults to native on supported Linux hosts and Docker elsewhere.",
        file=stream,
    )


def _runtime(value: str) -> str:
    if value == "auto":
        if platform.system() == "Linux" and platform.machine() in {
            "aarch64", "arm64", "x86_64", "amd64"
        }:
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
    env["OPENURMA_LAB_ROOT"] = (
        env.get("OPENURMA_CONTAINER_LAB_ROOT", "/workspace/openurma-gem5-lab")
        if runtime == "docker" else str(ROOT)
    )
    return subprocess.run(["bash", str(path), *args], env=env).returncode


def _build(target: str, args: Sequence[str], runtime: str) -> int:
    relative = BUILD_TARGETS[target]
    # Most component build scripts are intentionally Linux-native.  Make the
    # public CLI perform the container boundary instead of requiring callers
    # to know an internal docker-exec incantation.  The few orchestrator
    # scripts below already use scripts/runtime.sh and must remain on the host.
    host_orchestrated = {"ns3ub", "qemu", "mooncake", "mooncake-initramfs"}
    if runtime != "docker" or target in host_orchestrated:
        return _script(relative, args, "native" if target == "qemu" else runtime)

    container = os.environ.get("OPENURMA_CONTAINER", "openurma-gem5-lab")
    container_root = os.environ.get(
        "OPENURMA_CONTAINER_LAB_ROOT", "/workspace/openurma-gem5-lab"
    )
    command = [
        "docker", "exec",
        "-e", "OPENURMA_EXECUTION_MODE=native",
        "-e", f"OPENURMA_LAB_ROOT={container_root}",
        container,
        "bash", f"{container_root}/{relative}", *args,
    ]
    return subprocess.run(command).returncode


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
    env["OPENURMA_LAB_ROOT"] = (
        env.get("OPENURMA_CONTAINER_LAB_ROOT", "/workspace/openurma-gem5-lab")
        if runtime == "docker" else str(ROOT)
    )
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
    if command == "attach-qemu":
        return _script("scripts/run/attach-qemu.sh", tail, "native")
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
        return _build(tail[0], tail[1:], runtime)
    if command in COMMANDS:
        qemu_host_commands = {
            "start-qemu", "start-qemu-dual", "status-qemu", "stop-qemu",
            "qemu-rma-regression",
        }
        command_runtime = "native" if command in qemu_host_commands else runtime
        return _script(COMMANDS[command].script, tail, command_runtime)

    print(f"lab: unknown command: {command}", file=sys.stderr)
    _usage(sys.stderr)
    return 2
