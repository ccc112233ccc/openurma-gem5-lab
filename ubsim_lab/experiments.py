"""Saved single-port URMA experiments for the local workbench."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

from .cli import ROOT, _parse_manifest, _select_compatible_checkpoint
from .topology import compile_topology
from .qemu_ports import qemu_ports


OUTPUT = ROOT / "experiments" / "ui-workbench"
OPS = ("read_bw", "write_bw", "read_lat", "write_lat")
DEFAULT_RECIPE = {"operations": ["read_bw"], "sizes": [2, 32, 128, 512, 2048, 4096],
                  "jettys": [1], "total_ops": 16384, "warmups": 1, "repeats": 3,
                  "depth": 8192, "jfc_depth": 524288, "post_list": 256,
                  "timeout_s": 900}
FIELDS = ("sequence", "phase", "repeat", "operation", "size_bytes", "jettys",
          "total_operations", "peak_mib_s", "average_mib_s", "message_rate_mpps",
          "average_us", "p99_us", "p999_us", "p9999_us", "wall_seconds", "transcript")


def validate_recipe(raw: object, spec: dict) -> dict:
    if not isinstance(raw, dict) or set(raw) != set(DEFAULT_RECIPE):
        raise ValueError("实验配方字段不完整或包含未知字段")
    recipe = dict(raw)
    for field, choices, maximum in (("operations", OPS, 4), ("sizes", None, 24),
                                     ("jettys", None, 12)):
        values = recipe[field]
        if not isinstance(values, list) or not 1 <= len(values) <= maximum or len(set(map(str, values))) != len(values):
            raise ValueError(f"{field} 需要 1–{maximum} 个不重复值")
        if choices is not None and any(value not in choices for value in values):
            raise ValueError(f"{field} 包含不支持的测试")
    def integer(name: str, low: int, high: int) -> int:
        value = recipe[name]
        if type(value) is not int or not low <= value <= high:
            raise ValueError(f"{name} 需要位于 {low}–{high}")
        return value
    for value in recipe["sizes"]:
        if type(value) is not int or not 1 <= value <= 10 * 1024 * 1024:
            raise ValueError("消息大小需要位于 1B–10MiB")
    max_jettys = min(device.get("max_jettys", 47) for device in spec["devices"])
    for value in recipe["jettys"]:
        if type(value) is not int or not 1 <= value <= max_jettys:
            raise ValueError(f"Jetty 数量需要位于 1–{max_jettys}")
    if any(op.endswith("_lat") for op in recipe["operations"]) and recipe["jettys"] != [1]:
        raise ValueError("延迟测试目前只支持 1 个 Jetty；带宽配方可使用多个 Jetty")
    total = integer("total_ops", 256, 1_000_000)
    integer("warmups", 0, 5)
    integer("repeats", 1, 10)
    depth = integer("depth", 1, 8192)
    jfc = integer("jfc_depth", 1, 2097152)
    post = integer("post_list", 1, 256)
    integer("timeout_s", 10, 7200)
    if depth < post:
        raise ValueError("队列深度不能小于批量提交数")
    for j in recipe["jettys"]:
        if total % j or total // j < post:
            raise ValueError(f"总操作数需要被 J={j} 整除，且每 Jetty 不少于批量提交数")
        if jfc < j * min(depth, total // j):
            raise ValueError(f"JFC 深度不足以覆盖 J={j} 的并发窗口")
    if len(recipe["operations"]) * len(recipe["sizes"]) * len(recipe["jettys"]) * (recipe["warmups"] + recipe["repeats"]) > 120:
        raise ValueError("实验超过 120 次，请缩小配方")
    plan = compile_topology(spec)
    if len(plan["node_ids"]) != 2 or any(device["ports"] != 1 for device in spec["devices"]):
        raise ValueError("实验配方当前要求双机、每设备单端口")
    return recipe


def recipe_path(task_id: str) -> Path:
    from .web import TASKS, _task_path
    _task_path(task_id)
    return TASKS / f"{task_id}.recipe.json"


def read_recipe(task_id: str, spec: dict) -> dict:
    path = recipe_path(task_id)
    return validate_recipe(json.loads(path.read_text()) if path.is_file() else DEFAULT_RECIPE, spec)


def _ports(simulator: str) -> tuple[int, int]:
    names, defaults = (("UBSIM_DUAL_UART0", "UBSIM_DUAL_UART1"), (3460, 3470)) if simulator == "gem5" else (("UBSIM_QEMU_UART0", "UBSIM_QEMU_UART1"), (3560, 3570))
    return tuple(int(os.environ.get(name, default)) for name, default in zip(names, defaults))


def _port_free(port: int) -> bool:
    with socket.socket() as sock:
        try:
            sock.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def checkpoint_availability(spec: dict, plan: dict | None = None) -> dict:
    """Inspect the exact guest and network manifest without a shell probe."""
    plan = plan or compile_topology(spec)
    if plan["simulator"] != "gem5":
        return {"available": True, "checkpoint": None, "resolved": {},
                "launch_mode": "direct", "detail": "QEMU 可直接启动"}
    from .config_runtime import resolve_gem5
    resolved = resolve_gem5(spec, plan)
    checkpoint = (_select_compatible_checkpoint(resolved,
                                               image_path=resolved["initrd_path"])
                  if plan["checkpoint"] == "auto" else None)
    count = len(plan["node_ids"])
    detail = (f"匹配 {checkpoint}，可恢复启动" if checkpoint else
              f"{count} 台主机没有匹配 checkpoint，将从内核冷启动（启动前检查内存）"
              if plan["checkpoint"] == "auto" else
              f"{count} 台主机按配置冷启动（启动前检查内存）")
    return {"available": True, "checkpoint": checkpoint,
            "launch_mode": "restore" if checkpoint else "cold",
            "resolved": resolved, "detail": detail}


def preflight(spec: dict, runtime: dict, recipe: dict | None = None,
              terminal_busy: bool = False) -> dict:
    checks = []
    def check(label: str, ok: bool, detail: str) -> None:
        checks.append({"label": label, "ok": bool(ok), "detail": detail})
    plan = compile_topology(spec)
    simulator = plan["simulator"]
    check("拓扑", True, f"{len(plan['node_ids'])} 台主机，{plan['fabric_layout']}，单端口 {plan['node_links'][0]['rate_gbps']}Gbps")
    own = runtime[simulator]
    other = runtime["qemu" if simulator == "gem5" else "gem5"]
    same = own["active"] and own["topology_name"] == spec["name"]
    check("运行占用", not other["active"] and (not own["active"] or same),
          "当前任务已运行" if same else "端口与进程空闲" if not own["active"] and not other["active"] else "另一个拓扑正在运行")
    if same and recipe is not None:
        topology_file = Path(own["run_dir"]) / "topology.json"
        try:
            launched = json.loads(topology_file.read_text())
            matches = launched == spec
        except (OSError, json.JSONDecodeError):
            matches = False
        check("运行配置", matches, "与保存的拓扑完全一致" if matches else "运行中的拓扑参数已与保存配置不同，请重新启动")
    checkpoint = None
    if simulator == "gem5":
        availability = checkpoint_availability(spec, plan)
        checkpoint = availability["checkpoint"]
        check("启动方式", availability["available"] or same, availability["detail"])
        if availability["resolved"]:
            resolved = availability["resolved"]
            if not own["active"]:
                host = "macos" if sys.platform == "darwin" else "linux"
                runtime_host = "darwin" if sys.platform == "darwin" else "linux"
                arch = os.uname().machine
                if arch == "arm64" and host == "linux":
                    arch = "aarch64"
                defaults = [ROOT / ("gem5/build/ARM_MAC/gem5.opt" if host == "macos" else "gem5/build/ARM/gem5.opt"),
                            ROOT / "artifacts/kernel-64k-20261015/vmlinux", ROOT / "configs/arm64_fs.py",
                            ROOT / f"artifacts/udma-device-sim-build-{runtime_host}-{arch}/udma-device-sim",
                            ROOT / f"sources/ns-3-ub/build-{host}/scratch/ns3.44-ub-net-adapter"]
                paths = [Path(os.environ.get(name, default)) for name, default in zip(
                    ("UBSIM_GEM5", "UBSIM_KERNEL", "UBSIM_CONFIG", "UBSIM_UDMA_DEVICE_BINARY", "UBSIM_UB_SWITCH_BINARY"), defaults)]
                paths.append(Path(resolved.get("initrd_path", str(ROOT / "out/official-udma-64k-20261015.cpio.gz"))))
                missing = [str(path) for index, path in enumerate(paths)
                           if not path.is_file() or (index in (0, 3, 4) and not os.access(path, os.X_OK))]
                check("启动产物", not missing, "齐全" if not missing else "缺少：" + ", ".join(missing))
    else:
        host = "macos" if sys.platform == "darwin" else "linux"
        runtime_host = "darwin" if sys.platform == "darwin" else "linux"
        arch = os.uname().machine
        if arch == "arm64" and host == "linux":
            arch = "aarch64"
        tag = f"{runtime_host}-{arch}"
        required = [Path(os.environ.get("UBSIM_QEMU", ROOT / "sources/qemu-11.1.1/build/qemu-system-aarch64")),
                    Path(os.environ.get("UBSIM_QEMU_KERNEL", ROOT / "artifacts/kernel-64k-20261015/Image")),
                    Path(os.environ.get("UBSIM_INITRD", ROOT / "out/official-udma-64k-20261015.cpio.gz")),
                    Path(os.environ.get("UBSIM_UDMA_DEVICE_BINARY", ROOT / f"artifacts/udma-device-sim-build-{tag}/udma-device-sim")),
                    Path(os.environ.get("UBSIM_NS3UB_ADAPTER", ROOT / f"sources/ns-3-ub/build-{host}/scratch/ns3.44-ub-net-adapter"))]
        if not required[3].is_file():
            required[3] = ROOT / "artifacts/udma-device-sim-build/udma-device-sim"
        missing = [str(path) for index, path in enumerate(required)
                   if not path.is_file() or (index in (0, 3, 4) and not os.access(path, os.X_OK))]
        check("启动产物", not missing, "齐全" if not missing else "缺少：" + ", ".join(missing))
    if not own["active"]:
        modeled_memory = (availability["resolved"]["memory_size"] if simulator == "gem5"
                          else f"{max(machine['memory_mib'] for machine in plan['machines'])}MiB")
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/host_memory_guard.py"), "check",
             "--phase", "start" if simulator == "gem5" else "qemu-start",
             "--nodes", str(len(plan["node_ids"])), "--mem-size", modeled_memory],
            capture_output=True, text=True, check=False)
        detail = (result.stdout + result.stderr).strip().replace("\n", "；")
        check("宿主内存", result.returncode == 0,
              detail if detail else "无法读取宿主内存余量")
    if not own["active"]:
        base, second = _ports(simulator)
        ports = [base + index * (second - base) for index in range(len(plan["node_ids"]))]
        if simulator == "qemu":
            try:
                port_map = qemu_ports(len(plan["node_ids"]))
                ports = port_map["uart"] + port_map["ssh"] + port_map["oob"]
            except ValueError as exc:
                check("本地端口", False, str(exc))
                ports = []
        if ports:
            occupied = [str(port) for port in ports if not _port_free(port)]
            check("本地端口", not occupied, "空闲：" + ", ".join(map(str, ports)) if not occupied else "已占用：" + ", ".join(occupied))
    if recipe is not None:
        try:
            validate_recipe(recipe, spec)
            check("实验配方", True, "字段、Jetty、队列深度和测试规模有效")
        except ValueError as exc:
            check("实验配方", False, str(exc))
        check("实验主机", same, "当前拓扑运行中" if same else "请先启动当前任务")
        if simulator == "gem5":
            timing = (all(node.get("profile") == "modern" for node in spec["nodes"]) and
                      spec["execution"]["sync"] == "required")
            check("gem5 计时", timing, "modern + ROI 同步" if timing else "性能实验要求 modern CPU 与 required 同步")
        if same and simulator == "gem5":
            ready = (Path(own["run_dir"]) / "sync.ready").is_file()
            check("双机网络", True, "已完成 ./lab sync" if ready else "运行实验时自动执行 ./lab sync")
        check("串口会话", not terminal_busy, "可用于实验" if not terminal_busy else "请先关闭页面中的主机终端")
    return {"ok": all(item["ok"] for item in checks), "checks": checks,
            "checkpoint": checkpoint, "simulator": simulator, "running_this_task": same}


def command(op: str, size: int, jettys: int, recipe: dict, port: int,
            client: bool, simulator: str) -> str:
    parts = []
    if simulator == "gem5":
        parts += ["UBSIM_DIST_SYNC=1", "OPENURMA_DIST_SYNC=1"]
    parts += ["LD_LIBRARY_PATH=/lib:/usr/lib", "urma_perftest", op,
              "-d", "udma0", "--eid_idx", "0", "--ctp", "-s", str(size),
              "-P", str(port), "-J", str(jettys)]
    if op.endswith("_bw"):
        parts += ["--jettys_pre_jfr", "1", "-I", "0"]
    else:
        parts += ["-I", "128"]
    parts += ["-T", str(recipe["depth"]), "-R", str(recipe["depth"]),
              "-C", str(recipe["jfc_depth"]),
              "-n", str(recipe["total_ops"] // jettys), "-Q", "1", "-p", "0"]
    if op.endswith("_bw"):
        parts += ["-l", str(recipe["post_list"])]
    if client:
        parts += ["-S", "10.0.0.1"]
    return " ".join(parts)


def parse_output(output: str, op: str, size: int, jettys: int, total_ops: int) -> dict:
    exits = re.findall(r"__UBSIM_RC_\d+_\d+__=(\d+)", output)
    counts = re.findall(r"Number of jettys\s*:\s*(\d+)", output)
    if exits != ["0", "0"] or counts != [str(jettys)] * 2:
        raise ValueError(f"端点状态或 Jetty 数不匹配：rc={exits}, jettys={counts}")
    number = r"[0-9]+(?:\.[0-9]+)?"
    if op.endswith("_bw"):
        rows = re.findall(rf"(?m)^\s*{size}\s+{total_ops}\s+({number})\s+({number})\s+({number})\s*$", output)
        if len(rows) != 2 or rows[0] != rows[1]:
            raise ValueError(f"带宽结果应由两端给出同一行，实际得到 {len(rows)} 行")
        peak, average, mpps = map(float, rows[0])
        return {"peak_mib_s": peak, "average_mib_s": average, "message_rate_mpps": mpps}
    rows = re.findall(rf"(?m)^\s*{size}\s+(\d+)\s+({number})\s+({number})\s+({number})\s+({number})\s+({number})\s+({number})\s+({number})\s+({number})\s+({number})\s*$", output)
    if len(rows) != 1:
        raise ValueError(f"延迟结果应有一行，实际得到 {len(rows)} 行")
    values = rows[0]
    return {"samples": int(values[0]), "average_us": float(values[4]),
            "p99_us": float(values[6]), "p999_us": float(values[7]),
            "p9999_us": float(values[8])}


def summarize(rows: list[dict]) -> list[dict]:
    grouped: dict[tuple, list[dict]] = {}
    for row in rows:
        if row["phase"] == "measure":
            grouped.setdefault((row["operation"], row["size_bytes"], row["jettys"]), []).append(row)
    output = []
    for (op, size, jettys), samples in sorted(grouped.items()):
        metric = "average_mib_s" if op.endswith("_bw") else "average_us"
        value = sum(float(row[metric]) for row in samples) / len(samples)
        item = {"operation": op, "size_bytes": size, "jettys": jettys,
                "repeats": len(samples), "metric": metric, "value": value,
                "minimum": min(float(row[metric]) for row in samples),
                "maximum": max(float(row[metric]) for row in samples)}
        if op.endswith("_bw"):
            item["message_rate_mpps"] = sum(float(row["message_rate_mpps"]) for row in samples) / len(samples)
        output.append(item)
    return output


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run(task_id: str, directory: Path) -> int:
    from .web import _read_task, _run_dir, _runtime, _atomic_json
    spec = _read_task(task_id)
    recipe = read_recipe(task_id, spec)
    plan = compile_topology(spec)
    simulator = plan["simulator"]
    runtime = {name: _runtime(name) for name in ("gem5", "qemu")}
    verdict = preflight(spec, runtime, recipe)
    if not verdict["ok"]:
        raise ValueError("；".join(item["detail"] for item in verdict["checks"] if not item["ok"]))
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "topology.json").write_text(json.dumps(spec, ensure_ascii=False, indent=2) + "\n")
    (directory / "recipe.json").write_text(json.dumps(recipe, ensure_ascii=False, indent=2) + "\n")
    run_root = _run_dir(simulator)
    for filename in ("run-manifest.txt", "topology-plan.json"):
        source = run_root / filename
        if source.is_file():
            shutil.copy2(source, directory / filename)
    manifest = _parse_manifest((run_root / "run-manifest.txt").read_text()) if (run_root / "run-manifest.txt").is_file() else {}
    checkpoint = manifest.get("restore_checkpoint")
    if checkpoint and checkpoint != "none":
        source = ROOT / "checkpoints" / checkpoint / "checkpoint-manifest.txt"
        if source.is_file():
            shutil.copy2(source, directory / "checkpoint-manifest.txt")
    default_image = ROOT / "out/official-udma-64k-20261015.cpio.gz"
    image_field = "initrd_path" if simulator == "gem5" else "initrd_image"
    image = Path(manifest.get(image_field, os.environ.get("UBSIM_INITRD", str(default_image))))
    provenance = {"task_id": task_id, "simulator": simulator, "started_at": time.time(),
                  "run_root": str(run_root), "checkpoint": checkpoint,
                  "image": str(image), "image_sha256": _sha256(image) if image.is_file() else None}
    _atomic_json(directory / "provenance.json", provenance)
    status = {"state": "running", "task_id": task_id, "pid": os.getpid(), "completed": 0,
              "total": len(recipe["operations"]) * len(recipe["sizes"]) * len(recipe["jettys"]) * (recipe["warmups"] + recipe["repeats"]),
              "started_at": provenance["started_at"]}
    _atomic_json(directory / "status.json", status)
    rows = []
    ports = _ports(simulator)
    if simulator == "gem5" and not (run_root / "sync.ready").is_file():
        setup = subprocess.run([sys.executable, str(ROOT / "lab"), "sync"],
                               cwd=ROOT, capture_output=True, text=True, timeout=1900)
        (directory / "network-setup.log").write_text(setup.stdout + setup.stderr)
        if setup.returncode or not (run_root / "sync.ready").is_file():
            raise RuntimeError("gem5 双机网络初始化失败，详见 network-setup.log")
    if simulator == "qemu":
        setup = subprocess.run([sys.executable, str(ROOT / "tools/dual_serial_command.py"),
                                "--ports", *map(str, ports), "--command", "ubsim-net-up",
                                "--timeout", "120", "--prompt-kick-after", "1"],
                               cwd=ROOT, capture_output=True, text=True, timeout=140)
        (directory / "network-setup.uart.txt").write_text(setup.stdout + setup.stderr)
        if setup.returncode:
            raise RuntimeError("QEMU 网络初始化失败，详见 network-setup.uart.txt")
    for op in recipe["operations"]:
        for size in recipe["sizes"]:
            for jettys in recipe["jettys"]:
                for index in range(recipe["warmups"] + recipe["repeats"]):
                    sequence = len(rows) + 1
                    phase = "warmup" if index < recipe["warmups"] else "measure"
                    port = 21600 + (sequence - 1) % 1000
                    commands = [command(op, size, jettys, recipe, port, False, simulator),
                                command(op, size, jettys, recipe, port, True, simulator)]
                    transcript = f"{sequence:03d}-{op}-s{size}-j{jettys}-{phase}.uart.txt"
                    if simulator == "gem5":
                        from tools.run_modular_rma_regression import collect_fence_deltas, fence_logs, validate_fences
                        paths = fence_logs(run_root)
                        offsets = {name: path.stat().st_size if path.is_file() else None
                                   for name, path in paths.items()}
                    started = time.perf_counter()
                    result = subprocess.run([sys.executable, str(ROOT / "tools/dual_serial_command.py"),
                                             "--ports", *map(str, ports), "--commands", *commands,
                                             "--timeout", str(recipe["timeout_s"]),
                                             "--prompt-kick-after", "1", "--stagger", "1", "--full-output"],
                                            cwd=ROOT, capture_output=True, text=True,
                                            timeout=recipe["timeout_s"] + 20)
                    elapsed = time.perf_counter() - started
                    output = result.stdout + result.stderr
                    (directory / transcript).write_text(output)
                    if result.returncode:
                        raise RuntimeError(f"{transcript} 失败，退出码 {result.returncode}")
                    if simulator == "gem5":
                        logs, errors = collect_fence_deltas(paths, offsets, directory, transcript.removesuffix(".uart.txt") + ".fences")
                        errors.extend(validate_fences(logs))
                        if errors:
                            raise RuntimeError(f"同步围栏校验失败：{'; '.join(errors)}")
                    metrics = parse_output(output, op, size, jettys, recipe["total_ops"])
                    row = {"sequence": sequence, "phase": phase,
                           "repeat": max(0, index - recipe["warmups"] + 1),
                           "operation": op, "size_bytes": size, "jettys": jettys,
                           "total_operations": recipe["total_ops"], **metrics,
                           "wall_seconds": round(elapsed, 3), "transcript": transcript}
                    rows.append(row)
                    with (directory / "results.csv").open("w", newline="") as stream:
                        writer = csv.DictWriter(stream, fieldnames=FIELDS, extrasaction="ignore")
                        writer.writeheader()
                        writer.writerows(rows)
                    _atomic_json(directory / "summary.json", {"rows": summarize(rows)})
                    status["completed"] = sequence
                    status["current"] = f"{op} · {size}B · J={jettys} · {phase}"
                    _atomic_json(directory / "status.json", status)
                    print(status["current"], flush=True)
    status.update({"state": "finished", "ended_at": time.time()})
    _atomic_json(directory / "status.json", status)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("task_id")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        return run(args.task_id, args.output)
    except Exception as exc:
        args.output.mkdir(parents=True, exist_ok=True)
        status_path = args.output / "status.json"
        status = json.loads(status_path.read_text()) if status_path.is_file() else {}
        status.update({"state": "failed", "error": str(exc), "ended_at": time.time()})
        from .web import _atomic_json
        _atomic_json(status_path, status)
        print(f"experiment failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
