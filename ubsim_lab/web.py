"""Local browser workbench for declared UB simulation topologies.

The UI is deliberately a thin layer over ``compile_topology`` and ``./lab
topology start``. It never invents a second topology schema or simulator path.
"""

from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import json
import os
import re
import select
import socket
import socketserver
import subprocess
import sys
import threading
import time
import uuid
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from .cli import ROOT
from . import experiments
from .topology import TopologyError, compile_topology


STATIC = Path(__file__).parent / "web_static"
TASKS = ROOT / "configs" / "ui-tasks"
MAX_BODY = 256 * 1024
TASK_ID = re.compile(r"^(?:template-[a-z0-9-]+|task-[a-z0-9-]+)$")
TEMPLATES = {
    ("gem5", "switch"): "topology-gem5-two-heads.json",
    ("qemu", "switch"): "topology-qemu-two-heads.json",
}
TEMPLATE_LABELS = {
    "topology-gem5-two-heads.json": "gem5 · 双机经交换芯片",
    "topology-qemu-two-heads.json": "QEMU · 双机经交换芯片",
}


def _task_path(task_id: str) -> Path:
    if not TASK_ID.fullmatch(task_id):
        raise ValueError("无效的任务 ID")
    if task_id.startswith("template-"):
        name = task_id.removeprefix("template-") + ".json"
        if name not in TEMPLATES.values():
            raise ValueError("找不到该模板")
        return ROOT / "configs" / name
    return TASKS / (task_id + ".json")


def _metadata_path(task_id: str) -> Path:
    return TASKS / (task_id + ".meta.json")


def _task_metadata(task_id: str) -> dict:
    try:
        return json.loads(_metadata_path(task_id).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _read_task(task_id: str) -> dict:
    path = _task_path(task_id)
    if not path.is_file():
        raise ValueError("找不到该任务")
    return json.loads(path.read_text(encoding="utf-8"))


def _display_name(task_id: str, spec: dict) -> str:
    if task_id.startswith("template-"):
        return TEMPLATE_LABELS.get(_task_path(task_id).name, spec["name"])
    return _task_metadata(task_id).get("display_name", spec["name"])


def _summary(task_id: str) -> dict:
    spec = _read_task(task_id)
    return {
        "id": task_id,
        "name": _display_name(task_id, spec),
        "simulator": spec["nodes"][0]["simulator"],
        "layout": "switch" if spec["switches"] else "direct",
        "node_count": len(spec["nodes"]),
        "editable": task_id.startswith("task-"),
        "topology_name": spec["name"],
    }


def _list_tasks() -> list[dict]:
    templates = ["template-" + name.removesuffix(".json") for name in TEMPLATES.values()]
    saved = sorted((p for p in TASKS.glob("task-*.json")
                    if not p.name.endswith((".meta.json", ".recipe.json"))),
                   key=lambda p: p.stat().st_mtime, reverse=True) if TASKS.exists() else []
    return [_summary(p.stem) for p in saved] + [_summary(task_id) for task_id in templates]


def _atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n",
                             encoding="utf-8")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def _resize_single_leaf(spec: dict, host_count: object) -> dict:
    """Generate the supported star, keeping parameters on existing hosts and links."""
    compile_topology(spec)
    if len(spec["switches"]) != 1:
        raise ValueError("规模调整只支持单叶交换机拓扑")
    simulator = spec["nodes"][0]["simulator"]
    maximum = 8
    if type(host_count) is not int or not 2 <= host_count <= maximum:
        raise ValueError(f"{simulator} 单叶拓扑支持 2–{maximum} 台主机")
    result = copy.deepcopy(spec)
    current_nodes = {node["id"]: node for node in result["nodes"]}
    current_devices = {device["node"]: device for device in result["devices"]}
    current_links = {(link["device"], link["device_port"]): link
                     for link in result["links"]}
    node_seed = result["nodes"][0]
    device_seed = result["devices"][0]
    link_seed = result["links"][0]
    result["nodes"] = []
    result["devices"] = []
    result["links"] = []
    for index in range(host_count):
        suffix = chr(ord("a") + index)
        node_id = f"head-{suffix}"
        device_id = f"udma-{suffix}"
        node = copy.deepcopy(current_nodes.get(node_id, node_seed))
        node["id"] = node_id
        device = copy.deepcopy(current_devices.get(node_id, device_seed))
        device.update(id=device_id, node=node_id, eid=256 + index)
        result["nodes"].append(node)
        result["devices"].append(device)
        for port in range(device["ports"]):
            link = copy.deepcopy(current_links.get((device_id, port), link_seed))
            link.update(device=device_id, device_port=port,
                        switch=result["switches"][0]["id"],
                        switch_port=index * device["ports"] + port)
            result["links"].append(link)
    compile_topology(result)
    return result


def _template_spec(simulator: object, host_count: object) -> dict:
    template = TEMPLATES.get((simulator, "switch"))
    if template is None:
        raise ValueError("不支持该仿真器")
    spec = json.loads((ROOT / "configs" / template).read_text(encoding="utf-8"))
    if simulator == "gem5":
        # Keep one machine specification as the topology grows or shrinks.
        # The 4-GiB default also fits the local host's multi-node memory guard.
        spec.setdefault("machine", {}).setdefault("memory_size_gb", 4)
    return _resize_single_leaf(spec, host_count)


def _host_details(spec: dict) -> list[dict]:
    """Expose effective host hardware values for the topology inspector."""
    plan = compile_topology(spec)
    if plan["simulator"] == "gem5":
        from .config_runtime import resolve_gem5
        machine = resolve_gem5(spec, plan)
        custom_kernel = bool(os.environ.get("UBSIM_KERNEL"))
        release_file = ROOT / "artifacts/kernel-64k-20261015/kernelrelease.txt"
        release = (release_file.read_text().strip() if not custom_kernel and
                   release_file.is_file() else "版本待确认")
        detail = {
            "cpu_model": machine["cpu_roi_model"],
            "boot_cpu_model": machine["cpu_boot_model"],
            "cpu_freq": machine["cpu_freq"],
            "cpu_count": machine["cpu_count"],
            "l1i": machine["l1i_size"], "l1d": machine["l1d_size"],
            "l2": machine["l2_size"],
            "l3": machine["l3_size"] if machine["last_cache_level"] == "3" else None,
            "memory_size": machine["memory_size"],
            "guest_memory_limit": machine["guest_memory_limit"],
            "memory_type": machine["memory_type"],
            "memory_channels": machine["memory_channels"],
            "os": f"Linux {release} · initramfs",
            "page_size": "64 KiB" if not custom_kernel else None,
        }
        return [dict(detail) for _ in spec["nodes"]]
    custom_kernel = bool(os.environ.get("UBSIM_QEMU_KERNEL"))
    release_file = ROOT / "artifacts/kernel-64k-20261015/kernelrelease.txt"
    release = (release_file.read_text().strip() if not custom_kernel and
               release_file.is_file() else "版本待确认")
    return [{"cpu_model": node.get("cpu", "cortex-a72"),
             "cpu_count": node.get("cores", 1),
             "cpu_freq": None, "l1i": None, "l1d": None, "l2": None,
             "l3": None, "memory_size": f"{node.get('memory_mib', 1024)} MiB",
             "guest_memory_limit": None, "memory_type": None,
             "memory_channels": None,
             "os": f"Linux {release} · initramfs",
             "page_size": "64 KiB" if not custom_kernel else None}
            for node in spec["nodes"]]


def _create_task(body: dict) -> dict:
    display_name = body.get("name")
    simulator = body.get("simulator")
    topology_type = body.get("topology_type")
    host_count = body.get("host_count")
    if not isinstance(display_name, str) or not 1 <= len(display_name.strip()) <= 80:
        raise ValueError("任务名称需要 1–80 个字符")
    if topology_type != "single_leaf":
        raise ValueError("当前只支持单叶交换机拓扑")
    spec = _template_spec(simulator, host_count)
    task_id = "task-" + time.strftime("%Y%m%d%H%M%S") + "-" + uuid.uuid4().hex[:8]
    spec["name"] = task_id.replace("-", "_")
    compile_topology(spec)
    _atomic_json(_task_path(task_id), spec)
    _atomic_json(_metadata_path(task_id), {"display_name": display_name.strip()})
    return _summary(task_id)


def _copy_task(task_id: str) -> dict:
    source = _read_task(task_id)
    clone = copy.deepcopy(source)
    new_id = "task-" + time.strftime("%Y%m%d%H%M%S") + "-" + uuid.uuid4().hex[:8]
    clone["name"] = new_id.replace("-", "_")
    compile_topology(clone)
    display_name = (_display_name(task_id, source) + " 副本")[:80]
    _atomic_json(_task_path(new_id), clone)
    _atomic_json(_metadata_path(new_id), {"display_name": display_name})
    source_recipe = experiments.recipe_path(task_id)
    if source_recipe.is_file() and len(clone["nodes"]) == 2:
        recipe = experiments.validate_recipe(json.loads(source_recipe.read_text()), clone)
        _atomic_json(experiments.recipe_path(new_id), recipe)
    return _summary(new_id)


def _save_task(task_id: str, body: dict) -> dict:
    if not task_id.startswith("task-"):
        raise ValueError("内置模板不可修改；请先新建任务")
    current = _read_task(task_id)
    spec = body.get("spec")
    if not isinstance(spec, dict):
        raise ValueError("缺少拓扑配置")
    spec = copy.deepcopy(spec)
    if spec.get("name") != current["name"]:
        raise ValueError("任务内部名称不可修改")
    compile_topology(spec)
    display_name = body.get("display_name", _display_name(task_id, current))
    if not isinstance(display_name, str) or not 1 <= len(display_name.strip()) <= 80:
        raise ValueError("任务名称需要 1–80 个字符")
    _atomic_json(_task_path(task_id), spec)
    _atomic_json(_metadata_path(task_id), {"display_name": display_name.strip()})
    return _summary(task_id)


def _run_dir(simulator: str) -> Path:
    env_name, default = (("UBSIM_DUAL_OUT", "run-dual") if simulator == "gem5"
                         else ("UBSIM_QEMU_DUAL_OUT", "run-qemu-dual"))
    return Path(os.environ.get(env_name, str(ROOT / default)))


def _pid_alive(path: Path) -> bool:
    try:
        pid = int(path.read_text(encoding="ascii").splitlines()[0])
        os.kill(pid, 0)
        return True
    except (OSError, ValueError, IndexError):
        return False


def _process_alive(pid: object) -> bool:
    try:
        os.kill(int(pid), 0)
        return True
    except (OSError, TypeError, ValueError):
        return False


def _experiment_dir(run_id: str) -> Path:
    if not re.fullmatch(r"run-[0-9]{8}-[0-9]{6}-[a-f0-9]{6}", run_id):
        raise ValueError("无效的实验 ID")
    return experiments.OUTPUT / run_id


def _runtime(simulator: str) -> dict:
    run_dir = _run_dir(simulator)
    pidfile = run_dir / "node0" / ("gem5.pid" if simulator == "gem5" else "qemu.pid")
    active = _pid_alive(pidfile)
    try:
        topology = json.loads((run_dir / "topology.json").read_text(encoding="utf-8"))
        topology_name = topology.get("name")
    except (OSError, ValueError):
        topology_name = None
    stop_reason = (run_dir / "memory-guard.stopped")
    return {"active": active, "topology_name": topology_name,
            "run_dir": str(run_dir), "simulator": simulator,
            "stop_reason": stop_reason.read_text().strip() if stop_reason.is_file() else None}


def _tail(path: Path, limit: int = 64 * 1024) -> str:
    try:
        with path.open("rb") as stream:
            stream.seek(0, os.SEEK_END)
            stream.seek(max(0, stream.tell() - limit))
            return stream.read().decode("utf-8", "replace")
    except OSError:
        return ""


class Workbench(ThreadingHTTPServer):
    daemon_threads = True

    def server_bind(self) -> None:
        # HTTPServer performs reverse DNS on the bind address. That lookup can
        # block for a long time on isolated development hosts.
        socketserver.TCPServer.server_bind(self)
        self.server_name = self.server_address[0]
        self.server_port = self.server_address[1]

    def __init__(self, address: tuple[str, int]):
        super().__init__(address, WorkbenchHandler)
        self.lock = threading.Lock()
        self.job: dict | None = None
        self.terminal_owners: set[tuple[str, int]] = set()

    def start_task(self, task_id: str) -> dict:
        spec = _read_task(task_id)
        plan = compile_topology(spec)
        verdict = self.preflight(task_id)
        if not verdict["ok"]:
            raise ValueError("；".join(item["detail"] for item in verdict["checks"] if not item["ok"]))
        with self.lock:
            if self.job and self.job.get("state") == "running":
                raise ValueError("已有任务正在启动")
            if any(_runtime(sim)["active"] for sim in ("gem5", "qemu")):
                raise ValueError("已有仿真在运行，请先停止现有任务")
            log_dir = ROOT / "out" / "ui-runs"
            log_dir.mkdir(parents=True, exist_ok=True)
            log_path = log_dir / (task_id + "-" + time.strftime("%Y%m%d-%H%M%S") + ".log")
            env = os.environ.copy()
            with log_path.open("wb") as log:
                process = subprocess.Popen(
                    [sys.executable, str(ROOT / "lab"), "topology", "start",
                     str(_task_path(task_id))], cwd=ROOT, env=env,
                    stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            job = {"id": uuid.uuid4().hex, "task_id": task_id,
                   "simulator": plan["simulator"], "state": "running",
                   "started_at": time.time(), "log_path": str(log_path)}
            self.job = job
        def wait() -> None:
            code = process.wait()
            with self.lock:
                job["state"] = "finished" if code == 0 else "failed"
                job["returncode"] = code
                job["ended_at"] = time.time()
        threading.Thread(target=wait, daemon=True).start()
        return self.job_status()

    def preflight(self, task_id: str, experiment: bool = False) -> dict:
        spec = _read_task(task_id)
        recipe = experiments.read_recipe(task_id, spec) if experiment else None
        runtime = {sim: _runtime(sim) for sim in ("gem5", "qemu")}
        verdict = experiments.preflight(spec, runtime, recipe,
                                         bool(self.terminal_owners) if experiment else False)
        if not experiment and verdict["running_this_task"]:
            verdict["checks"].append({"label": "启动状态", "ok": False,
                                      "detail": "当前任务已经运行"})
            verdict["ok"] = False
        if self.job and self.job.get("state") == "running":
            verdict["checks"].append({"label": "启动任务", "ok": False,
                                      "detail": "另一个任务仍在启动"})
            verdict["ok"] = False
        return verdict

    def start_experiment(self, task_id: str) -> dict:
        verdict = self.preflight(task_id, experiment=True)
        if not verdict["ok"]:
            raise ValueError("；".join(item["detail"] for item in verdict["checks"] if not item["ok"]))
        with self.lock:
            for previous in experiments.OUTPUT.glob("*/status.json"):
                try:
                    status = json.loads(previous.read_text())
                    if status.get("state") == "running" and _process_alive(status.get("pid")):
                        raise ValueError("已有性能实验在运行")
                except (OSError, json.JSONDecodeError):
                    continue
            run_id = "run-" + time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6]
            directory = experiments.OUTPUT / run_id
            experiments.OUTPUT.mkdir(parents=True, exist_ok=True)
            directory.mkdir()
            _atomic_json(directory / "status.json", {"state": "starting", "task_id": task_id,
                                                     "completed": 0, "started_at": time.time()})
            log_path = experiments.OUTPUT / (run_id + ".log")
            with log_path.open("wb") as log:
                process = subprocess.Popen([sys.executable, "-m", "ubsim_lab.experiments",
                                            task_id, str(directory)], cwd=ROOT,
                                           stdout=log, stderr=subprocess.STDOUT,
                                           start_new_session=True)
            # The worker writes its own PID with the first durable state update.
        return self.experiment_detail(run_id)

    def experiment_detail(self, run_id: str) -> dict:
        directory = _experiment_dir(run_id)
        status_path = directory / "status.json"
        if not status_path.is_file():
            raise ValueError("找不到实验")
        status = json.loads(status_path.read_text())
        if status.get("state") == "running" and not _process_alive(status.get("pid")):
            status.update({"state": "failed", "error": "实验进程已结束，未写入完成状态"})
            _atomic_json(status_path, status)
        summary = json.loads((directory / "summary.json").read_text()) if (directory / "summary.json").is_file() else {"rows": []}
        return {"id": run_id, **status, "summary": summary["rows"],
                "log": _tail(experiments.OUTPUT / (run_id + ".log"), 12000),
                "directory": str(directory)}

    def job_status(self) -> dict | None:
        with self.lock:
            job = self.job
            if job is None:
                return None
            return {key: value for key, value in job.items() if key != "log_path"} | {
                "log": _tail(Path(job["log_path"])),
            }


class IPv6Workbench(Workbench):
    address_family = socket.AF_INET6


class WorkbenchHandler(BaseHTTPRequestHandler):
    server: Workbench
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args: object) -> None:
        sys.stderr.write("ui: " + format % args + "\n")

    def _json(self, status: HTTPStatus, value: object) -> None:
        data = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(data)

    def _error(self, status: HTTPStatus, message: str) -> None:
        self._json(status, {"error": message})

    def _origin_ok(self) -> bool:
        origin = self.headers.get("Origin")
        if not origin:
            return True
        expected = f"http://{self.headers.get('Host', '')}"
        return origin == expected

    def _body(self) -> dict:
        if self.headers.get("Content-Type", "").split(";", 1)[0] != "application/json":
            raise ValueError("请求需要 application/json")
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as exc:
            raise ValueError("无效的请求长度") from exc
        if not 0 < length <= MAX_BODY:
            raise ValueError("请求内容过大或为空")
        value = json.loads(self.rfile.read(length))
        if not isinstance(value, dict):
            raise ValueError("请求内容必须为 JSON 对象")
        return value

    def do_GET(self) -> None:
        parts = urlsplit(self.path)
        path = parts.path
        try:
            if path in ("/", "/index.html", "/app.css", "/app.js"):
                name = "index.html" if path == "/" else path.lstrip("/")
                content = (STATIC / name).read_bytes()
                mime = ("text/html" if name.endswith("html") else
                        "text/css" if name.endswith("css") else "text/javascript")
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", mime + "; charset=utf-8")
                self.send_header("Content-Length", str(len(content)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.end_headers()
                self.wfile.write(content)
            elif path == "/api/tasks":
                self._json(HTTPStatus.OK, {"tasks": _list_tasks()})
            elif path.startswith("/api/tasks/") and path.endswith("/recipe"):
                task_id = path.split("/")[3]
                spec = _read_task(task_id)
                if len(spec["nodes"]) != 2:
                    self._json(HTTPStatus.OK, {"recipe": None,
                                               "reason": "性能实验当前只支持双机拓扑"})
                else:
                    self._json(HTTPStatus.OK, {"recipe": experiments.read_recipe(task_id, spec)})
            elif path.startswith("/api/tasks/") and path.endswith("/preflight"):
                task_id = path.split("/")[3]
                experiment = parse_qs(parts.query).get("experiment", ["0"])[0] == "1"
                self._json(HTTPStatus.OK, self.server.preflight(task_id, experiment))
            elif path.startswith("/api/tasks/") and path.count("/") == 3:
                task_id = path.rsplit("/", 1)[1]
                spec = _read_task(task_id)
                self._json(HTTPStatus.OK, {"task": _summary(task_id), "spec": spec,
                                           "plan": compile_topology(spec)})
            elif path == "/api/runtime":
                self._json(HTTPStatus.OK, {"gem5": _runtime("gem5"),
                                           "qemu": _runtime("qemu"),
                                           "job": self.server.job_status()})
            elif path == "/api/experiments":
                task_id = parse_qs(parts.query).get("task", [""])[0]
                _read_task(task_id)
                runs = []
                for directory in sorted(experiments.OUTPUT.glob("run-*/status.json"), reverse=True):
                    try:
                        status = json.loads(directory.read_text())
                        if status.get("task_id") == task_id:
                            runs.append({"id": directory.parent.name, **status})
                    except (OSError, json.JSONDecodeError):
                        continue
                self._json(HTTPStatus.OK, {"runs": runs[:50]})
            elif path.startswith("/api/experiments/") and path.endswith("/results.csv"):
                run_id = path.split("/")[3]
                csv_path = _experiment_dir(run_id) / "results.csv"
                data = csv_path.read_bytes()
                self.send_response(HTTPStatus.OK)
                self.send_header("Content-Type", "text/csv; charset=utf-8")
                self.send_header("Content-Disposition", f'attachment; filename="{run_id}.csv"')
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(data)
            elif path.startswith("/api/experiments/") and path.count("/") == 3:
                self._json(HTTPStatus.OK, self.server.experiment_detail(path.rsplit("/", 1)[1]))
            elif path == "/api/terminal":
                params = parse_qs(parts.query)
                task_id = params.get("task", [""])[0]
                node_id = params.get("node", [""])[0]
                spec = _read_task(task_id)
                index = next((i for i, node in enumerate(spec["nodes"])
                              if node["id"] == node_id), None)
                if index is None:
                    raise ValueError("找不到该主机")
                simulator = spec["nodes"][0]["simulator"]
                runtime = _runtime(simulator)
                log = _tail(_run_dir(simulator) / f"node{index}" / "system.terminal", 24 * 1024)
                current = runtime["topology_name"] == spec["name"]
                pidfile = _run_dir(simulator) / f"node{index}" / ("gem5.pid" if simulator == "gem5" else "qemu.pid")
                self._json(HTTPStatus.OK, {"text": log if current else "",
                                           "live": current and _pid_alive(pidfile),
                                           "node_index": index, "simulator": simulator})
            elif path == "/ws/terminal":
                self._terminal_socket(parts.query)
            else:
                self._error(HTTPStatus.NOT_FOUND, "找不到页面")
        except (OSError, ValueError, TopologyError, json.JSONDecodeError,
                subprocess.TimeoutExpired) as exc:
            self._error(HTTPStatus.BAD_REQUEST, str(exc))

    def do_POST(self) -> None:
        if not self._origin_ok():
            self._error(HTTPStatus.FORBIDDEN, "请求来源不匹配")
            return
        path = urlsplit(self.path).path
        try:
            if path == "/api/tasks":
                self._json(HTTPStatus.CREATED, {"task": _create_task(self._body())})
            elif path == "/api/topology/checkpoint":
                body = self._body()
                spec = body.get("spec")
                if spec is None:
                    spec = _template_spec(body.get("simulator"), body.get("host_count"))
                status = experiments.checkpoint_availability(spec)
                self._json(HTTPStatus.OK, {key: status[key] for key in
                                           ("available", "checkpoint", "launch_mode", "detail")} |
                           {"hosts": _host_details(spec)})
            elif path == "/api/topology/resize":
                body = self._body()
                self._json(HTTPStatus.OK, {"spec": _resize_single_leaf(
                    body.get("spec"), body.get("host_count"))})
            elif path.startswith("/api/tasks/") and path.endswith("/copy"):
                task_id = path.split("/")[3]
                self._body()
                self._json(HTTPStatus.CREATED, {"task": _copy_task(task_id)})
            elif path.startswith("/api/tasks/") and path.endswith("/start"):
                task_id = path.split("/")[3]
                self._body()
                self._json(HTTPStatus.ACCEPTED, {"job": self.server.start_task(task_id)})
            elif path.startswith("/api/tasks/") and path.endswith("/experiment"):
                task_id = path.split("/")[3]
                self._body()
                self._json(HTTPStatus.ACCEPTED, {"run": self.server.start_experiment(task_id)})
            elif path == "/api/stop":
                body = self._body()
                spec = _read_task(body.get("task_id", ""))
                simulator = spec["nodes"][0]["simulator"]
                current = _runtime(simulator)
                if not current["active"] or current["topology_name"] != spec["name"]:
                    raise ValueError("该任务当前没有运行中的仿真")
                command = "stop" if simulator == "gem5" else "stop-qemu"
                result = subprocess.run([sys.executable, str(ROOT / "lab"), command],
                                        cwd=ROOT, capture_output=True, text=True, timeout=45)
                self._json(HTTPStatus.OK if result.returncode == 0 else HTTPStatus.CONFLICT,
                           {"returncode": result.returncode,
                            "output": (result.stdout + result.stderr)[-16000:]})
            else:
                self._error(HTTPStatus.NOT_FOUND, "找不到接口")
        except (OSError, ValueError, TopologyError, json.JSONDecodeError,
                subprocess.TimeoutExpired) as exc:
            self._error(HTTPStatus.BAD_REQUEST, str(exc))

    def do_PUT(self) -> None:
        if not self._origin_ok():
            self._error(HTTPStatus.FORBIDDEN, "请求来源不匹配")
            return
        path = urlsplit(self.path).path
        try:
            if path.startswith("/api/tasks/") and path.count("/") == 3:
                task_id = path.rsplit("/", 1)[1]
                self._json(HTTPStatus.OK, {"task": _save_task(task_id, self._body())})
            elif path.startswith("/api/tasks/") and path.endswith("/recipe"):
                task_id = path.split("/")[3]
                spec = _read_task(task_id)
                recipe = experiments.validate_recipe(self._body(), spec)
                _atomic_json(experiments.recipe_path(task_id), recipe)
                self._json(HTTPStatus.OK, {"recipe": recipe})
            else:
                self._error(HTTPStatus.NOT_FOUND, "找不到接口")
        except (OSError, ValueError, TopologyError, json.JSONDecodeError) as exc:
            self._error(HTTPStatus.BAD_REQUEST, str(exc))

    def _terminal_socket(self, query: str) -> None:
        if not self._origin_ok():
            self._error(HTTPStatus.FORBIDDEN, "请求来源不匹配")
            return
        params = parse_qs(query)
        try:
            task_id = params.get("task", [""])[0]
            node_id = params.get("node", [""])[0]
            spec = _read_task(task_id)
            index = next((i for i, node in enumerate(spec["nodes"])
                          if node["id"] == node_id), None)
            if index is None:
                raise ValueError("找不到该主机")
            simulator = spec["nodes"][0]["simulator"]
            runtime = _runtime(simulator)
            if not runtime["active"] or runtime["topology_name"] != spec["name"]:
                raise ValueError("该任务当前没有运行中的终端")
            key = (simulator, index)
            with self.server.lock:
                if key in self.server.terminal_owners:
                    raise ValueError("该主机终端已在另一个浏览器窗口连接")
                self.server.terminal_owners.add(key)
            try:
                base = int(os.environ.get("UBSIM_DUAL_UART0" if simulator == "gem5"
                                          else "UBSIM_QEMU_UART0", "3460" if simulator == "gem5" else "3560"))
                second = int(os.environ.get("UBSIM_DUAL_UART1" if simulator == "gem5"
                                            else "UBSIM_QEMU_UART1", "3470" if simulator == "gem5" else "3570"))
                port = base + (second - base) * index
                with socket.create_connection(("127.0.0.1", port), timeout=3) as guest:
                    ws_key = self.headers.get("Sec-WebSocket-Key", "")
                    if self.headers.get("Upgrade", "").lower() != "websocket" or not ws_key:
                        raise ValueError("需要 WebSocket 连接")
                    accept = base64.b64encode(hashlib.sha1(
                        (ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")
                    ).digest()).decode("ascii")
                    self.send_response(HTTPStatus.SWITCHING_PROTOCOLS)
                    self.send_header("Upgrade", "websocket")
                    self.send_header("Connection", "Upgrade")
                    self.send_header("Sec-WebSocket-Accept", accept)
                    self.end_headers()
                    self.close_connection = True
                    pending = bytearray()
                    while True:
                        readable, _, _ = select.select([self.connection, guest], [], [], 1)
                        if guest in readable:
                            data = guest.recv(4096)
                            if not data:
                                break
                            self.connection.sendall(_ws_frame(data))
                        if self.connection in readable:
                            data = self.connection.recv(4096)
                            if not data:
                                break
                            pending.extend(data)
                            if len(pending) > 16384:
                                break
                            for opcode, payload in _ws_parse(pending):
                                if opcode == 8:
                                    return
                                if opcode in (1, 2):
                                    guest.sendall(payload)
            finally:
                with self.server.lock:
                    self.server.terminal_owners.discard(key)
        except (OSError, ValueError, UnicodeEncodeError) as exc:
            if not self.close_connection:
                self._error(HTTPStatus.BAD_REQUEST, str(exc))


def _ws_frame(data: bytes) -> bytes:
    length = len(data)
    if length < 126:
        return bytes((0x82, length)) + data
    return bytes((0x82, 126)) + length.to_bytes(2, "big") + data


def _ws_parse(buffer: bytearray) -> list[tuple[int, bytes]]:
    frames: list[tuple[int, bytes]] = []
    while len(buffer) >= 2:
        first, second = buffer[:2]
        masked = bool(second & 0x80)
        length = second & 0x7f
        offset = 2
        if length == 126:
            if len(buffer) < 4:
                break
            length = int.from_bytes(buffer[2:4], "big")
            offset = 4
        elif length == 127:
            if len(buffer) < 10:
                break
            length = int.from_bytes(buffer[2:10], "big")
            offset = 10
        if not masked or length > 8192:
            raise ValueError("无效的终端消息")
        if len(buffer) < offset + 4 + length:
            break
        mask = buffer[offset:offset + 4]
        payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(
            buffer[offset + 4:offset + 4 + length]))
        frames.append((first & 0x0f, payload))
        del buffer[:offset + 4 + length]
    return frames


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="本机 UB 仿真工作台")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args(argv)
    if args.host not in ("127.0.0.1", "localhost", "::1"):
        parser.error("工作台仅允许绑定本机回环地址")
    server_type = IPv6Workbench if args.host == "::1" else Workbench
    with server_type((args.host, args.port)) as server:
        host, port = server.server_address[:2]
        print(f"UB 仿真工作台：http://{host}:{port}/", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
