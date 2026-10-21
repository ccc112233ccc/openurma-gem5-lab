"""Native process launcher for validated JSON topologies.

Legacy ``./lab start`` remains available for hand-tuned shell experiments;
``./lab topology start`` uses this module and never invokes that shell path.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time

from .cli import ROOT, _select_compatible_checkpoint
from .config_runtime import resolve_gem5
from .qemu_ports import qemu_ports


class LaunchError(RuntimeError):
    pass


GEM5_OPTIONS = """
cpu m5ops-base cpu-freq num-cpus benchmark-cpu o3-width o3-rob-entries
o3-iq-entries o3-lq-entries o3-sq-entries o3-load-ports o3-store-ports
o3-fetch-buffer-bytes o3-fetch-queue-entries o3-phys-int-regs o3-phys-float-regs
o3-phys-vec-regs o3-phys-vec-pred-regs o3-phys-mat-regs last-cache-level
cache-line-size l1i-size l1i-assoc l1i-latency l1i-mshrs l1i-targets-per-mshr
l1i-write-buffers l1d-size l1d-prefetcher l1d-assoc l1d-latency l1d-mshrs
l1d-targets-per-mshr l1d-write-buffers l2-size l2-assoc l2-latency l2-mshrs
l2-targets-per-mshr l2-write-buffers l3-size l3-assoc l3-latency l3-mshrs
l3-targets-per-mshr l3-write-buffers fabric-freq fabric-width-bytes
coherent-bus-frontend-latency coherent-bus-forward-latency
coherent-bus-response-latency coherent-bus-snoop-response-latency
coherent-bus-header-latency memory-bus-frontend-latency
memory-bus-forward-latency memory-bus-response-latency
memory-bus-snoop-response-latency memory-bus-header-latency
core-bus-snoop-filter-capacity l3-bus-snoop-filter-capacity
membus-snoop-filter-capacity io-bus-frontend-latency io-bus-forward-latency
io-bus-response-latency io-bus-header-latency io-cache-size io-cache-assoc
io-cache-latency io-cache-mshrs io-cache-targets-per-mshr io-cache-write-buffers
mem-size guest-mem-limit mem-type mem-channels mem-channels-intlv
mem-addr-mapping xor-low-bit mem-ranks mem-read-buffer-size mem-write-buffer-size
mem-page-policy mem-max-accesses-per-row mem-sched-policy mem-write-high-thresh
mem-write-low-thresh mem-min-writes-per-switch mem-min-reads-per-switch
mem-ctrl-frontend-latency mem-ctrl-backend-latency mem-ctrl-command-window
""".split()

GEM5_KEY_OVERRIDES = {
    "cpu": "cpu_mode", "num-cpus": "cpu_count",
    "l1d-prefetcher": "cache_prefetcher",
    "mem-size": "memory_size", "guest-mem-limit": "guest_memory_limit",
    "mem-type": "memory_type", "mem-channels-intlv": "memory_channel_interleave_bytes",
    "mem-addr-mapping": "memory_address_mapping",
    "xor-low-bit": "memory_channel_xor_low_bit",
    "mem-ranks": "memory_ranks_per_channel",
    "mem-read-buffer-size": "memory_read_buffer_bursts_per_channel",
    "mem-write-buffer-size": "memory_write_buffer_bursts_per_channel",
    "mem-sched-policy": "memory_scheduler",
    "mem-write-high-thresh": "memory_write_high_threshold_percent",
    "mem-write-low-thresh": "memory_write_low_threshold_percent",
    "mem-ctrl-frontend-latency": "memory_controller_frontend_latency",
    "mem-ctrl-backend-latency": "memory_controller_backend_latency",
    "mem-ctrl-command-window": "memory_controller_command_window",
}


def _gem5_key(option: str) -> str:
    if option in GEM5_KEY_OVERRIDES:
        return GEM5_KEY_OVERRIDES[option]
    key = option.replace("-", "_")
    if key.startswith(("l1i_", "l1d_", "l2_", "l3_", "io_cache_")) and key.endswith("_latency"):
        return key + "_tag_data_response_cycles"
    if key.startswith(("coherent_bus_", "memory_bus_", "io_bus_")) and key.endswith("_latency"):
        return key + "_cycles"
    if key == "mem_channels":
        return "memory_channels"
    if key.startswith("mem_"):
        return "memory_" + key[4:]
    return key


def gem5_command(manifest: dict[str, str], node: int, run_root: Path,
                 checkpoint: Path | None, template: bool,
                 write_identity: bool = True) -> list[str]:
    out = run_root / f"node{node}"
    gem5 = Path(os.environ.get("UBSIM_GEM5", str(ROOT / (
        "gem5/build/ARM_MAC/gem5.opt" if sys.platform == "darwin" else
        "gem5/build/ARM/gem5.opt"))))
    config = Path(os.environ.get("UBSIM_CONFIG", str(ROOT / "configs/arm64_fs.py")))
    kernel = Path(os.environ.get("UBSIM_KERNEL", str(ROOT / "artifacts/kernel-64k-20261015/vmlinux")))
    initrd = Path(manifest["initrd_path"])
    script = str(out / "identity.txt") if template else os.environ.get("UBSIM_GUEST_READFILE", "")
    if template and write_identity:
        Path(script).write_text(f"{node}\n")
    command = [str(gem5), "--listener-mode=on", f"--outdir={out}",
               str(config), f"--kernel={kernel}", f"--initrd={initrd}",
               "--root-device=/dev/ram", f"--script={script}"]
    for option in GEM5_OPTIONS:
        key = _gem5_key(option)
        command.append(f"--{option}={manifest[key]}")
    command.extend(["--dist-size=0"])
    if manifest["auto_o3_lifecycle"] == "1":
        command.append("--auto-o3-lifecycle")
    if manifest["fast_boot_noncaching"] == "1":
        command.append("--fast-boot-noncaching")
    if checkpoint is not None:
        source = checkpoint / ("template-cpt" if template else f"node{node}-cpt")
        command.append(f"--restore-from={source}")
    uart0 = int(os.environ.get("UBSIM_DUAL_UART0", "3460"))
    uart1 = int(os.environ.get("UBSIM_DUAL_UART1", "3470"))
    uart = uart0 + node * (uart1 - uart0)
    mac = f"02:00:00:00:00:{node + 1:02x}"
    tap = (os.environ.get("UBSIM_DUAL_TAP0", "/tmp/ubsim-dual.eth0.sock") if node == 0
           else os.environ.get("UBSIM_DUAL_TAP1", "/tmp/ubsim-dual.eth1.sock") if node == 1
           else f"/tmp/ubsim-dual.eth{node}.sock")
    guest_node = "template" if template else str(node)
    command.extend([
        f"--eth-tap-socket={tap}", f"--eth-link-speed={manifest['oob_link_speed']}",
        f"--eth-link-delay={manifest['peer_latency_ns']}ns",
        f"--terminal-port={uart}", f"--eth-mac={mac}",
        f"--extra-cmdline=ubsim_node={guest_node} " +
        ("ubsim_template=1 " if template else "") +
        f"ubsim_provider={manifest['provider']}",
    ])
    return command


def _live_pid(pidfile: Path) -> bool:
    try:
        pid = int(pidfile.read_text().strip())
        os.kill(pid, 0)
        return True
    except (OSError, ValueError):
        return False


def _spawn(command: list[str], pidfile: Path, logfile: Path,
           env: dict[str, str] | None, started: list[subprocess.Popen]) -> None:
    pidfile.parent.mkdir(parents=True, exist_ok=True)
    with logfile.open("wb") as log:
        process = subprocess.Popen(["nohup", *command], stdin=subprocess.DEVNULL, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True,
                                   env=env, cwd=ROOT)
    pidfile.write_text(f"{process.pid}\n")
    started.append(process)


def _wait_socket(path: Path, seconds: float = 5.0) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if path.is_socket():
            return
        time.sleep(0.05)
    raise LaunchError(f"UDMA socket did not appear: {path}")


def _check_memory(phase: str, nodes: int, mem_size: str) -> None:
    result = subprocess.run([sys.executable, str(ROOT / "tools/host_memory_guard.py"),
                             "check", "--phase", phase, "--nodes", str(nodes),
                             "--mem-size", mem_size], check=False)
    if result.returncode:
        raise LaunchError("host memory preflight failed")


def _require_files(paths: list[Path]) -> None:
    for path in paths:
        if not path.is_file():
            raise LaunchError(f"missing required artifact: {path}")


def _clear_owned_run_paths(run_root: Path, count: int) -> None:
    if run_root == Path("/") or run_root == Path("."):
        raise LaunchError(f"unsafe run output directory: {run_root}")
    for name in ("switch", "ub-switch", "oob-switch", "ub-fabric",
                 "run-manifest.txt", "sync.ready", "memory-guard.pid",
                 "memory-guard.log", "memory-guard.stopped",
                 "memory-guard.stderr.log"):
        path = run_root / name
        if path.is_dir():
            shutil.rmtree(path)
        elif path.exists():
            path.unlink()
    for prefix in ("node", "udma-node"):
        for node in range(8):
            path = run_root / f"{prefix}{node}"
            if path.is_dir():
                shutil.rmtree(path)


def start_gem5(spec: dict, plan: dict) -> Path:
    manifest = resolve_gem5(spec, plan)
    selected = (_select_compatible_checkpoint(manifest,
                                              image_path=manifest["initrd_path"])
                if plan["checkpoint"] == "auto" else None)
    checkpoint = ROOT / "checkpoints" / selected if selected else None
    template = bool(checkpoint and (checkpoint / "checkpoint-manifest.txt").is_file() and
                    "checkpoint_kind=node-template-v1" in
                    (checkpoint / "checkpoint-manifest.txt").read_text())
    if template and manifest["profile"] != "modern":
        raise LaunchError("节点模板目前仅支持 modern profile")
    manifest["restore_checkpoint"] = selected or "none"
    count = len(plan["node_ids"])
    run_root = Path(os.environ.get("UBSIM_DUAL_OUT", str(ROOT / "run-dual")))
    if any(_live_pid(path) for path in run_root.glob("**/*.pid")):
        raise LaunchError("已有仿真进程运行，请先停止")
    _check_memory("start", count, manifest["memory_size"])
    host = "macos" if sys.platform == "darwin" else "linux"
    arch = os.uname().machine
    if arch == "arm64" and host == "linux":
        arch = "aarch64"
    udma = Path(os.environ.get("UBSIM_UDMA_DEVICE_BINARY", str(
        ROOT / f"artifacts/udma-device-sim-build-{'darwin' if host == 'macos' else 'linux'}-{arch}/udma-device-sim")))
    ns3 = Path(os.environ.get("UBSIM_UB_SWITCH_BINARY", str(
        ROOT / f"sources/ns-3-ub/build-{host}/scratch/ns3.44-ub-net-adapter")))
    sample_gem5 = gem5_command(manifest, 0, run_root, checkpoint, template,
                                write_identity=False)
    _require_files([Path(sample_gem5[0]), Path(sample_gem5[3]),
                    Path(manifest["initrd_path"]), udma, ns3,
                    Path(sample_gem5[4].split("=", 1)[1])])
    _clear_owned_run_paths(run_root, count)
    for node in range(count):
        (run_root / f"node{node}").mkdir(parents=True, exist_ok=True)
        (run_root / f"udma-node{node}").mkdir(parents=True, exist_ok=True)
    for directory in ("ub-switch", "oob-switch"):
        (run_root / directory).mkdir(parents=True, exist_ok=True)
    (run_root / "run-manifest.txt").write_text("".join(
        f"{key}={value}\n" for key, value in manifest.items()))
    started: list[subprocess.Popen] = []
    sockets = [Path(f"/tmp/ubsim-dual.node{node}.{kind}.sock")
               for node in range(count) for kind in ("ub-host", "ub-net")]
    sockets += [Path(f"/tmp/ubsim-dual.node{node}.udma.shm") for node in range(count)]
    sockets += [Path(os.environ.get("UBSIM_DUAL_TAP0", "/tmp/ubsim-dual.eth0.sock")),
                Path(os.environ.get("UBSIM_DUAL_TAP1", "/tmp/ubsim-dual.eth1.sock"))]
    sockets += [Path(f"/tmp/ubsim-dual.eth{node}.sock") for node in range(2, count)]
    for path in sockets:
        path.unlink(missing_ok=True)
    sync = "required" if manifest["virtual_time_synchronization"] == "enabled" else "off"
    try:
        for node in range(count):
            host_socket = f"/tmp/ubsim-dual.node{node}.ub-host.sock"
            net_socket = f"/tmp/ubsim-dual.node{node}.ub-net.sock"
            args = [str(udma), "--host-socket", host_socket, "--net-socket", net_socket,
                    "--shm", f"/tmp/ubsim-dual.node{node}.udma.shm", "--sync", sync,
                    "--host-link-latency-ps", str(int(manifest["external_udma_host_latency_ns"]) * 1000),
                    "--net-link-latency-ps", str(plan["node_links"][node]["latency_ns"] * 1000),
                    "--sync-interval-ps", str(int(manifest["sync_quantum_ns"]) * 1000),
                    "--eid", str(0x100 + node), "--ports", manifest["ub_port_count"]]
            for flag, key in (("max-jfs-depth", "max_jfs_depth"),
                              ("max-jfr-depth", "max_jfr_depth"),
                              ("max-jfc-depth", "max_jfc_depth"),
                              ("max-jettys", "max_jettys"),
                              ("public-jettys", "public_jettys"),
                              ("sq-prefetch-wqebbs", "sq_prefetch_wqebbs"),
                              ("sq-fetch-burst-wqebbs", "sq_fetch_burst_wqebbs"),
                              ("sq-fetch-max-outstanding", "sq_fetch_max_outstanding"),
                              ("completion-event-slots", "completion_event_slots"),
                              ("write-payload-read-window", "write_payload_read_window")):
                args.extend((f"--{flag}", manifest[key]))
            if manifest["profile"] == "modern":
                args.extend(("--one-shot-jfc-notifications", "--pipelined-completion-dma"))
            if int(manifest["max_read_outstanding"]):
                args.extend(("--max-read-outstanding", manifest["max_read_outstanding"]))
            if manifest["lifecycle_sync"] == "1":
                args.append("--lifecycle-sync")
            if checkpoint is not None and not template:
                args.extend(("--state-in", str(checkpoint / f"udma-node{node}.state")))
            args.extend(("--state-out", str(run_root / f"udma-node{node}/state.bin")))
            _spawn(args, run_root / f"udma-node{node}/udma.pid",
                   run_root / f"udma-node{node}/udma.log", None, started)
        for path in sockets[:count * 2]:
            _wait_socket(path)
        def picoseconds(value: str) -> int:
            for unit, scale in (("ps", 1), ("ns", 1000), ("us", 1000000),
                                ("ms", 1000000000), ("s", 1000000000000)):
                if value.endswith(unit):
                    return int(value[:-len(unit)]) * scale
            raise LaunchError(f"invalid delay: {value}")
        ns3_args = [str(ns3), "--ports", manifest["ub_port_count"],
                    "--link-delay-ps", str(int(manifest["peer_latency_ns"]) * 1000),
                    "--switch-delay-ps", str(picoseconds(manifest["peer_switch_delay"])),
                    "--rate-gbps", manifest["peer_link_rate_gbps"], "--sync", sync,
                    "--sync-interval-ps", str(int(manifest["sync_quantum_ns"]) * 1000),
                    "--ctp-retransmission", manifest["ctp_retransmission"],
                    "--ctp-rto-ps", manifest["ctp_rto_ps"],
                    "--ctp-max-retransmissions", manifest["ctp_max_retransmissions"]]
        if plan["fabric_layout"] == "direct":
            ns3_args.append("--direct-link")
        if manifest["lifecycle_sync"] == "1":
            ns3_args.append("--lifecycle-sync")
        for node, link in enumerate(plan["node_links"]):
            ns3_args.extend(("--endpoint", f"/tmp/ubsim-dual.node{node}.ub-net.sock,"
                             f"{0x100 + node},{link['latency_ns'] * 1000},{link['rate_gbps']}"))
        _spawn(ns3_args, run_root / "ub-switch/gem5.pid",
               run_root / "ub-switch/gem5.log", None, started)
        for node in range(count):
            env = os.environ.copy()
            env.update({
                "M5_PATH": os.environ.get("UBSIM_M5_PATH", str(ROOT / "system")),
                "UBSIM_PIPE_DATA": manifest["pipe_data"],
                "UBSIM_TRACE_PACKETS": manifest["packet_trace"],
                "UBSIM_ADAPTER_LOCAL_SYNC": "1",
                "UBSIM_LIFECYCLE_SYNC": manifest["lifecycle_sync"],
                "OPENURMA_ADAPTER_LOCAL_SYNC": "1",
                "OPENURMA_LIFECYCLE_SYNC": manifest["lifecycle_sync"],
                "UBSIM_EXIT_AFTER_CHECKPOINT": "1",
                "UBSIM_UDMA_HOST_SOCKET": f"/tmp/ubsim-dual.node{node}.ub-host.sock",
                "UBSIM_UDMA_HOST_POLL_INTERVAL": manifest["external_udma_poll_interval"],
                "UBSIM_UDMA_HOST_SYNC": "1" if sync == "required" else "0",
                "UBSIM_UDMA_HOST_LIFECYCLE_SYNC": manifest["lifecycle_sync"],
                "UBSIM_UDMA_HOST_LINK_LATENCY": manifest["external_udma_host_latency_ns"] + "ns",
                "UBSIM_UDMA_HOST_SYNC_INTERVAL": manifest["sync_quantum_ns"] + "ns",
                "UBSIM_UDMA_HOST_PIO_LATENCY": str(2 * int(manifest["external_udma_host_latency_ns"])) + "ns",
            })
            command = gem5_command(manifest, node, run_root, checkpoint, template)
            _spawn(command, run_root / f"node{node}/gem5.pid",
                   run_root / f"node{node}/gem5.log", env, started)
        _spawn([sys.executable, str(ROOT / "tools/host_memory_guard.py"), "monitor",
                "--run-root", str(run_root), "--nodes", str(count),
                "--stop-script", str(ROOT / "scripts/run/stop-dual.sh")],
               run_root / "memory-guard.pid", run_root / "memory-guard.stderr.log",
               None, started)
        deadline = time.monotonic() + 10
        switch_log = run_root / "ub-switch/gem5.log"
        while time.monotonic() < deadline:
            if "[NS3_UB_NET] connected" in switch_log.read_text(errors="replace"):
                break
            time.sleep(0.05)
        else:
            raise LaunchError("ns-3 fabric did not connect; inspect switch and UDMA logs")
        taps = []
        for node in range(count):
            taps.append("unix:" + (os.environ.get("UBSIM_DUAL_TAP0", "/tmp/ubsim-dual.eth0.sock") if node == 0
                                   else os.environ.get("UBSIM_DUAL_TAP1", "/tmp/ubsim-dual.eth1.sock") if node == 1
                                   else f"/tmp/ubsim-dual.eth{node}.sock"))
        _spawn([sys.executable, str(ROOT / "tools/ethernet_relay.py"), *taps],
               run_root / "oob-switch/relay.pid", run_root / "oob-switch/relay.log",
               None, started)
    except BaseException:
        for process in reversed(started):
            process.terminate()
        raise
    if selected:
        print(f"Restored {count} gem5 nodes from checkpoint {selected}; run: {run_root}")
    else:
        print(f"Cold started {count} gem5 nodes from JSON; run: {run_root}")
    return run_root


def start_qemu(spec: dict, plan: dict) -> Path:
    """Launch a validated QEMU topology directly from its JSON."""
    count = len(plan["node_ids"])
    ports = qemu_ports(count)
    run_root = Path(os.environ.get("UBSIM_QEMU_DUAL_OUT", str(ROOT / "run-qemu-dual")))
    if any(_live_pid(path) for path in run_root.glob("**/*.pid")):
        raise LaunchError("已有 QEMU 仿真进程运行，请先停止")
    machines = plan["machines"]
    _check_memory("qemu-start", count,
                  f"{max(machine['memory_mib'] for machine in machines)}MiB")
    host = "macos" if sys.platform == "darwin" else "linux"
    arch = os.uname().machine
    if arch == "arm64" and host == "linux":
        arch = "aarch64"
    qemu = Path(os.environ.get("UBSIM_QEMU", str(ROOT / "sources/qemu-11.1.1/build/qemu-system-aarch64")))
    kernel = Path(os.environ.get("UBSIM_QEMU_KERNEL", str(ROOT / "artifacts/kernel-64k-20261015/Image")))
    initrd = Path(os.environ.get("UBSIM_INITRD", str(ROOT / "out/official-udma-64k-20261015.cpio.gz")))
    udma = Path(os.environ.get("UBSIM_UDMA_DEVICE_BINARY", str(
        ROOT / f"artifacts/udma-device-sim-build-{'darwin' if host == 'macos' else 'linux'}-{arch}/udma-device-sim")))
    if not udma.is_file():
        fallback = ROOT / "artifacts/udma-device-sim-build/udma-device-sim"
        if fallback.is_file():
            udma = fallback
    ns3 = Path(os.environ.get("UBSIM_NS3UB_ADAPTER", str(
        ROOT / f"sources/ns-3-ub/build-{host}/scratch/ns3.44-ub-net-adapter")))
    _require_files([qemu, kernel, initrd, udma, ns3])
    _clear_owned_run_paths(run_root, count)
    for name in ["ub-fabric", *[f"node{i}" for i in range(count)],
                 *[f"udma-node{i}" for i in range(count)],
                 *(["oob-switch"] if count > 2 else [])]:
        (run_root / name).mkdir(parents=True, exist_ok=True)
    uart0, uart1 = ports["uart"][:2]
    oob_port = ports["oob"][0]
    ssh_ports = ports["ssh"]
    timing = spec["execution"]["sync"] == "required"
    force_icount = os.environ.get("UBSIM_QEMU_ICOUNT_WITH_SYNC_OFF", "0")
    if force_icount not in ("0", "1"):
        raise LaunchError("UBSIM_QEMU_ICOUNT_WITH_SYNC_OFF must be 0 or 1")
    icount_enabled = timing or force_icount == "1"
    sync = "required" if timing else "off"
    mode = "timing" if timing else "icount-unsynchronized" if icount_enabled else "functional"
    poll_ns = "1000000" if timing else "1000"
    port_count = plan["port_count"]
    max_jettys = plan["max_jettys"]
    host_latency = int(os.environ.get("UBSIM_QEMU_HOST_LATENCY_NS", "500"))
    link_latency = min(link["latency_ns"] for link in plan["node_links"])
    rate = plan["node_links"][0]["rate_gbps"]
    switch_delay = plan["switch_delay_ns"]
    knobs = {
        "sq_prefetch_wqebbs": os.environ.get("UBSIM_SQ_PREFETCH_WQEBBS", "64"),
        "sq_fetch_burst_wqebbs": os.environ.get("UBSIM_SQ_FETCH_BURST_WQEBBS", "16"),
        "sq_fetch_max_outstanding": os.environ.get("UBSIM_SQ_FETCH_MAX_OUTSTANDING", "4"),
        "completion_event_slots": os.environ.get("UBSIM_COMPLETION_EVENT_SLOTS", "64"),
        "max_jfs_depth": os.environ.get("UBSIM_UDMA_MAX_JFS_DEPTH", "8192"),
        "max_jfr_depth": os.environ.get("UBSIM_UDMA_MAX_JFR_DEPTH", "8192"),
        "max_jfc_depth": os.environ.get("UBSIM_UDMA_MAX_JFC_DEPTH", "524288"),
        "dma_max_outstanding": os.environ.get("UBSIM_QEMU_DMA_MAX_OUTSTANDING", "256"),
        "iotlb_entries": os.environ.get("UBSIM_QEMU_IOTLB_ENTRIES", "4096"),
        "icount_shift": os.environ.get("UBSIM_QEMU_ICOUNT_SHIFT", "0"),
        "ctp_retransmission": os.environ.get("UBSIM_CTP_RETRANSMISSION", "off"),
        "ctp_rto_ps": os.environ.get("UBSIM_CTP_RTO_PS", "25600000"),
        "ctp_max_retransmissions": os.environ.get("UBSIM_CTP_MAX_RETRANSMISSIONS", "7"),
    }
    if knobs["ctp_retransmission"] == "auto":
        knobs["ctp_retransmission"] = "off"
    values = {
        "runtime": f"qemu-tcg-{mode}", "node_count": str(count),
        "network_backend": "modular-ns3ub", "ub_fabric_layout": plan["fabric_layout"],
        "synchronization": sync, "icount_enabled": str(int(icount_enabled)),
        "host_poll_ns": poll_ns,
        "uart0": str(uart0), "uart1": str(uart1),
        "oob_port": str(oob_port), "ssh0_port": str(ssh_ports[0]),
        "ssh1_port": str(ssh_ports[1]), "peer_link_rate_gbps": str(rate),
        "host_link_latency_ns": str(host_latency),
        "max_jettys": str(max_jettys), "public_jettys": "1",
        "udma_state_format": "3", "qemu_binary": str(qemu),
        "udma_device_binary": str(udma), "ns3_binary": str(ns3),
        "kernel_image": str(kernel), "initrd_image": str(initrd),
        "port_count": str(port_count), "peer_latency_ns": str(link_latency),
        "switch_delay_ns": str(switch_delay),
        **knobs,
    }
    for node, (machine, link) in enumerate(zip(machines, plan["node_links"])):
        values.update({f"node{node}_cpu_type": machine["cpu"],
                       f"node{node}_cpu_count": str(machine["cores"]),
                       f"node{node}_memory_mib": str(machine["memory_mib"]),
                       f"node{node}_uart_port": str(ports["uart"][node]),
                       f"node{node}_ssh_port": str(ports["ssh"][node]),
                       f"node{node}_oob_port": str(ports["oob"][node] if count > 2 else oob_port),
                       f"node{node}_link_rate_gbps": str(link["rate_gbps"]),
                       f"node{node}_link_latency_ns": str(link["latency_ns"])})
    for field, key in (("cpu_type", "cpu"), ("cpu_count", "cores"),
                       ("memory_mib", "memory_mib")):
        first = machines[0][key]
        values[field] = str(first) if all(machine[key] == first for machine in machines) else "mixed"
    values["oob_backend"] = "learning-relay" if count > 2 else "direct-socket"
    (run_root / "run-manifest.txt").write_text("".join(
        f"{key}={value}\n" for key, value in values.items()))
    sockets = [Path(f"/tmp/ubsim-qemu-dual.node{node}.{kind}")
               for node in range(count) for kind in ("host.sock", "net.sock", "shm")]
    for path in sockets:
        path.unlink(missing_ok=True)
    started: list[subprocess.Popen] = []
    try:
        for node, link in enumerate(plan["node_links"]):
            args = [str(udma), "--host-socket", f"/tmp/ubsim-qemu-dual.node{node}.host.sock",
                    "--net-socket", f"/tmp/ubsim-qemu-dual.node{node}.net.sock",
                    "--shm", f"/tmp/ubsim-qemu-dual.node{node}.shm",
                    "--sync", sync, "--host-link-latency-ps", str(host_latency * 1000),
                    "--net-link-latency-ps", str(link["latency_ns"] * 1000),
                    "--host-sync-interval-ps", str(host_latency * 1000),
                    "--net-sync-interval-ps", str(link["latency_ns"] * 1000),
                    "--eid", str(0x100 + node), "--ports", str(port_count),
                    "--dma-max-outstanding", knobs["dma_max_outstanding"]]
            for flag, key in (("sq-prefetch-wqebbs", "sq_prefetch_wqebbs"),
                              ("sq-fetch-burst-wqebbs", "sq_fetch_burst_wqebbs"),
                              ("sq-fetch-max-outstanding", "sq_fetch_max_outstanding"),
                              ("completion-event-slots", "completion_event_slots"),
                              ("max-jfs-depth", "max_jfs_depth"),
                              ("max-jfr-depth", "max_jfr_depth"),
                              ("max-jfc-depth", "max_jfc_depth")):
                args.extend((f"--{flag}", knobs[key]))
            args.extend(("--max-jettys", str(max_jettys), "--public-jettys", "1",
                         "--iotlb-entries", knobs["iotlb_entries"], "--state-out",
                         str(run_root / f"udma-node{node}/state.bin")))
            _spawn(args, run_root / f"udma-node{node}/udma.pid",
                   run_root / f"udma-node{node}/udma.log", None, started)
        for node in range(count):
            for kind in ("host", "net"):
                _wait_socket(Path(f"/tmp/ubsim-qemu-dual.node{node}.{kind}.sock"), 10)
        ns3_args = [str(ns3), "--ports", str(port_count),
                    "--link-delay-ps", str(link_latency * 1000),
                    "--switch-delay-ps", str(switch_delay * 1000),
                    "--rate-gbps", str(rate), "--sync", sync,
                    "--sync-interval-ps", str(link_latency * 1000),
                    "--ctp-retransmission", knobs["ctp_retransmission"],
                    "--ctp-rto-ps", knobs["ctp_rto_ps"],
                    "--ctp-max-retransmissions", knobs["ctp_max_retransmissions"]]
        if plan["fabric_layout"] == "direct":
            ns3_args.append("--direct-link")
        for node, link in enumerate(plan["node_links"]):
            ns3_args.extend(("--endpoint", f"/tmp/ubsim-qemu-dual.node{node}.net.sock,"
                             f"{0x100 + node},{link['latency_ns'] * 1000},{link['rate_gbps']}"))
        _spawn(ns3_args, run_root / "ub-fabric/ns3.pid",
               run_root / "ub-fabric/ns3.log", None, started)
        for node, machine in enumerate(machines):
            out = run_root / f"node{node}"
            (out / "system.terminal").touch()
            qemu_args = [str(qemu), "-machine", "virt,accel=tcg,gic-version=2,highmem=off",
                         "-cpu", machine["cpu"]]
            if icount_enabled:
                qemu_args.extend(("-icount", f"shift={knobs['icount_shift']},sleep=off"))
            qemu_args.extend((
                "-smp", str(machine["cores"]), "-m", str(machine["memory_mib"]),
                "-kernel", str(kernel), "-initrd", str(initrd),
                "-append", f"console=ttyAMA0 rdinit=/init nokaslr loglevel=5 "
                           f"ubsim_node={node} ubsim_provider=official ubsim_auto_net=1 ubsim_ssh=1",
                "-display", "none", "-monitor", "none", "-no-reboot",
                "-chardev", f"socket,id=uart0,host=127.0.0.1,"
                            f"port={ports['uart'][node]},server=on,wait=off,"
                            f"logfile={out}/system.terminal,logappend=on",
                "-serial", "chardev:uart0",
                "-device", f"e1000,netdev=oob,mac=02:00:00:00:00:{node+1:02x}",
                "-netdev", (f"socket,id=oob,listen=127.0.0.1:{ports['oob'][node]}" if count > 2
                            else f"socket,id=oob,listen=127.0.0.1:{oob_port}" if node == 0
                            else f"socket,id=oob,connect=127.0.0.1:{oob_port}"),
                "-device", f"e1000,netdev=ssh,mac=02:00:00:00:10:{node+1:02x}",
                "-netdev", f"user,id=ssh,hostfwd=tcp:127.0.0.1:{ssh_ports[node]}-:22",
            ))
            env = os.environ.copy()
            env.update({"UBSIM_QEMU_UB_HOST_SOCKET": f"/tmp/ubsim-qemu-dual.node{node}.host.sock",
                        "UBSIM_QEMU_UB_HOST_SYNC": sync,
                        "UBSIM_QEMU_UB_HOST_POLL_NS": poll_ns,
                        "UBSIM_QEMU_UB_HOST_LATENCY_PS": str(host_latency * 1000),
                        "UBSIM_QEMU_UB_HOST_SYNC_INTERVAL_PS": str(host_latency * 1000)})
            _spawn(qemu_args, out / "qemu.pid", out / "qemu.log", env, started)
            if node == 0:
                time.sleep(0.5)
        if count > 2:
            _spawn([sys.executable, str(ROOT / "tools/ethernet_relay.py"),
                    *[f"tcp:127.0.0.1:{port}" for port in ports["oob"]]],
                   run_root / "oob-switch/relay.pid",
                   run_root / "oob-switch/relay.log", None, started)
        _spawn([sys.executable, str(ROOT / "tools/host_memory_guard.py"), "monitor",
                "--run-root", str(run_root), "--nodes", str(count), "--pid-name", "qemu.pid",
                "--stop-script", str(ROOT / "scripts/run/stop-qemu-dual.sh")],
               run_root / "memory-guard.pid", run_root / "memory-guard.stderr.log",
               None, started)
    except BaseException:
        for process in reversed(started):
            process.terminate()
        raise
    print(f"Started {count} QEMU {mode} nodes from JSON; run: {run_root}")
    return run_root
