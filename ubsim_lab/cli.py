"""Unified CLI for building, launching, and operating the simulation lab.

Declarative ``topology start FILE.json`` launches native processes through
Python. Older imperative commands retain their shell backends for compatibility.
"""

from __future__ import annotations

import os
import hashlib
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
    "checkpoint-template": Command("scripts/run/checkpoint-template.sh", "save one node-neutral gem5 checkpoint"),
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

CHECKPOINT_IGNORED_MANIFEST_KEYS = {
    # Runtime/generated metadata which does not describe checkpointed machine
    # state and is expected to differ when the checkpoint is restored again.
    "restore_checkpoint",
    "execution_mode",
    "endpoint_eids",
    "peer_ring_bytes",
    "dist_link_speed",
    # A restored shell-ready checkpoint has no outstanding READs; this is an
    # ROI issue-credit setting rather than serialized guest/device state.
    "max_read_outstanding",
    # A quiescent ready checkpoint has no source payload DMA in flight.
    "write_payload_read_window",
    # The coordinated checkpoint saves gem5 and quiescent UDMA state, never
    # ns-3 fabric state. A new fabric is built on every restore, so these
    # topology-only settings may change while device ports/EIDs still match.
    "ub_fabric_layout",
    "peer_switch_delay",
    # The image contents are compared against checkpoint metadata below.
    "initrd_path",
}

BUILD_TARGETS = {
    "all": "scripts/build-all.sh",
    "gem5": "scripts/build/build_gem5.sh",
    "qemu": "scripts/build/build_qemu.sh",
    "kernel": "scripts/build/build_olk66.sh",
    "umdk": "scripts/build/build_umdk.sh",
    "initramfs": "official-udma/build_initramfs.sh",
    "template-initramfs": "scripts/build/build-template-initramfs.sh",
    "ns3ub": "scripts/build-ns3ub-adapter.sh",
    "udma-model": "scripts/build/build_udma_model.sh",
    "udma-device": "scripts/build/build_udma_device_sim.sh",
    "udma-optimized": "scripts/build/build_udma_optimized.sh",
    "optimized-initramfs": "scripts/build/build_udma_optimized_image.sh",
    "ub-switch": "scripts/build/build_ub_switch_sim.sh",
    "mooncake": "scripts/build-mooncake-urma.sh",
    "mooncake-initramfs": "scripts/package-mooncake-urma-initramfs.sh",
}


def _usage(stream=None) -> None:
    if stream is None:
        stream = sys.stdout
    print(
        "usage: ./lab [--runtime native] COMMAND [ARGS...]\n\n"
        "Lifecycle:\n"
        "  setup             fetch and build the reproducible environment\n"
        "  start             start N gem5 nodes plus the UB/OOB switches\n"
        "  start-ready       restore the newest compatible shell-ready checkpoint\n"
        "  topology ACTION FILE.json  validate, plan or start a declared topology\n"
        "  ui                open the local browser workbench\n"
        "  status            show simulator process and guest readiness\n"
        "  sync              initialize the guest control network\n"
        "  checkpoint NAME   save a shell-ready coordinated checkpoint\n"
        "  checkpoint-template NAME  save a reusable pre-identity checkpoint\n"
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
        "Use './lab COMMAND --help' for backend-specific options. Commands\n"
        "run on the host; Apple Silicon gem5 requires a native build.",
        file=stream,
    )


def _runtime(value: str) -> str:
    if value not in {"auto", "native"}:
        raise ValueError("only native runtime is supported")
    return "native"


def _split_global_options(argv: Sequence[str]) -> tuple[str, list[str]]:
    runtime = os.environ.get("UBSIM_EXECUTION_MODE", "auto")
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
    env["UBSIM_EXECUTION_MODE"] = runtime
    env["UBSIM_LAB_ROOT"] = env.get("UBSIM_LAB_ROOT", str(ROOT))
    return subprocess.run(["bash", str(path), *args], env=env).returncode


def _script_environment(runtime: str) -> dict[str, str]:
    env = os.environ.copy()
    env["UBSIM_EXECUTION_MODE"] = runtime
    env["UBSIM_LAB_ROOT"] = env.get("UBSIM_LAB_ROOT", str(ROOT))
    return env


def _parse_manifest(text: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for raw in text.splitlines():
        key, separator, value = raw.partition("=")
        if separator and key:
            result[key] = value
    return result


def _select_compatible_checkpoint(desired: dict[str, str],
                                  image_path: str | Path | None = None) -> str | None:
    checkpoint_root = ROOT / "checkpoints"
    compatible: list[tuple[str, str]] = []
    node_count = int(desired.get("node_count", "2"))
    # The initramfs is unpacked into guest RAM before a shell-ready checkpoint.
    # Matching only the machine manifest can silently restore an older guest
    # binary after rebuilding perftest or a provider.
    configured_image = (image_path if image_path is not None else
                        os.environ.get("UBSIM_INITRD") or desired.get("initrd_path"))
    if configured_image:
        container_root = Path("/workspace/ubsim-gem5-lab")
        image = Path(configured_image)
        if image.is_relative_to(container_root):
            image = ROOT / image.relative_to(container_root)
    else:
        image = ROOT / "out/official-udma-64k-20261015.cpio.gz"
    image_hash = None
    if image.is_file():
        digest = hashlib.sha256()
        with image.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        image_hash = digest.hexdigest()
    elif desired.get("profile") == "modern":
        return None
    for manifest_path in checkpoint_root.glob("*/run-manifest.txt"):
        manifest = _parse_manifest(manifest_path.read_text(errors="replace"))
        directory = manifest_path.parent
        metadata_path = directory / "checkpoint-manifest.txt"
        metadata = (
            _parse_manifest(metadata_path.read_text(errors="replace"))
            if metadata_path.is_file() else {}
        )
        template = metadata.get("checkpoint_kind") == "node-template-v1"
        keys = set(desired) - CHECKPOINT_IGNORED_MANIFEST_KEYS
        if template:
            keys.discard("node_count")
            keys.discard("template_checkpoint")
            keys = {key for key in keys if not (
                key.startswith("node") and
                (key.endswith("_link_latency_ns") or
                 key.endswith("_link_rate_gbps")))}
        # Fields introduced after the retained checkpoints use their original
        # defaults when absent. A hardware-depth checkpoint still cannot be
        # confused with an older 1024/4096 guest device state.
        defaults = {"auto_o3_lifecycle": "0", "fast_boot_noncaching": "0",
                    "max_jfs_depth": "1024", "max_jfr_depth": "1024",
                    "max_jfc_depth": "4096", "max_jettys": "1152",
                    "public_jettys": "128", "template_checkpoint": "0"}
        def mismatched(key: str) -> bool:
            value = manifest.get(key, defaults.get(key))
            # Older coordinated checkpoints used one uniform UB-link setting.
            # Their missing per-node fields are equivalent to that old global
            # value, while an asymmetric request still cannot match them.
            if value is None and key.startswith("node"):
                if key.endswith("_link_latency_ns"):
                    value = manifest.get("peer_latency_ns")
                elif key.endswith("_link_rate_gbps"):
                    value = manifest.get("peer_link_rate_gbps")
            # State v4 reads both v3 and v4. The old v3 guest checkpoint is
            # still valid, while a v3 reader must never select v4 state.
            if key == "udma_state_format" and desired[key] == "4" and value == "3":
                return False
            return value != desired[key]

        if any(mismatched(key) for key in keys):
            continue
        if template:
            required = [directory / "template-cpt/m5.cpt"]
        else:
            required = [directory / f"node{node}-cpt" for node in range(node_count)]
            required += [directory / f"udma-node{node}.state" for node in range(node_count)]
        if not all(path.exists() for path in required):
            continue
        if (image_hash is not None and
                (metadata.get("initramfs_sha256") is not None or
                 desired.get("profile") == "modern") and
                metadata.get("initramfs_sha256") != image_hash):
            continue
        compatible.append((metadata.get("created_utc", ""), directory.name))
    return max(compatible)[1] if compatible else None


def _start_ready(args: Sequence[str], runtime: str) -> int:
    if "--help" in args or "-h" in args:
        return _script(COMMANDS["start"].script, args, runtime)
    if any(value == "--restore-checkpoint" or
           value.startswith("--restore-checkpoint=") for value in args):
        print("lab: start-ready selects the checkpoint automatically; do not pass --restore-checkpoint",
              file=sys.stderr)
        return 2
    path = ROOT / COMMANDS["start"].script
    env = _script_environment(runtime)
    probe = subprocess.run(
        ["bash", str(path), *args, "--print-config"], env=env,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if probe.returncode != 0:
        sys.stderr.write(probe.stderr)
        return probe.returncode
    selected = _select_compatible_checkpoint(_parse_manifest(probe.stdout))
    if selected is None:
        print(
            "lab: no compatible shell-ready checkpoint; use './lab start ...' "
            "only when a cold boot is intentionally required",
            file=sys.stderr,
        )
        return 2
    if "--print-config" in args:
        print(f"start_ready_checkpoint={selected}")
        print(probe.stdout, end="")
        return 0
    print(f"start-ready: restoring compatible checkpoint '{selected}'", flush=True)
    return _script(COMMANDS["start"].script,
                   [*args, "--restore-checkpoint", selected], runtime)


def _build(target: str, args: Sequence[str], runtime: str) -> int:
    relative = BUILD_TARGETS[target]
    if (target == "gem5" and
            platform.system() == "Darwin" and platform.machine() == "arm64"):
        return _script("scripts/build/build_gem5_macos.sh", args, runtime)
    return _script(relative, args, runtime)


def _requested_target_arch(args: Sequence[str]) -> str | None:
    for index, value in enumerate(args):
        if value == "--target-arch" and index + 1 < len(args):
            return args[index + 1]
        if value.startswith("--target-arch="):
            return value.split("=", 1)[1]
    return None


def _attach(args: Sequence[str], runtime: str) -> int:
    if len(args) != 1 or not args[0].isdigit():
        print("usage: ./lab attach NODE", file=sys.stderr)
        return 2
    node = int(args[0])
    uart0 = int(os.environ.get("UBSIM_DUAL_UART0", "3460"))
    uart1 = int(os.environ.get("UBSIM_DUAL_UART1", "3470"))
    stride = uart1 - uart0
    if node < 0 or stride <= 0:
        print("lab: NODE must be non-negative and node1 UART must exceed node0 UART", file=sys.stderr)
        return 2
    env = os.environ.copy()
    env.setdefault("UBSIM_M5TERM_PORT", str(uart0 + node * stride))
    env["UBSIM_EXECUTION_MODE"] = runtime
    env["UBSIM_LAB_ROOT"] = env.get("UBSIM_LAB_ROOT", str(ROOT))
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
        return _script("scripts/setup-native.sh", tail, runtime)
    if command == "attach":
        return _attach(tail, runtime)
    if command == "attach-qemu":
        return _script("scripts/run/attach-qemu.sh", tail, "native")
    if command == "start-ready":
        return _start_ready(tail, runtime)
    if command == "topology":
        from .topology import run as run_topology
        return run_topology(tail, runtime, ROOT)
    if command == "ui":
        from .web import main as run_web
        return run_web(tail)
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
