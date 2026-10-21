"""Validate a declarative topology and compile an inspectable launch plan.

This is deliberately a strict adapter to the supported single-switch and direct backends.
Unsupported topology features fail before any simulator process is started.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any, Sequence


class TopologyError(ValueError):
    pass


QEMU_CPUS = {"cortex-a53", "cortex-a57", "cortex-a72", "cortex-a76",
             "cortex-a710", "neoverse-n1", "neoverse-n2", "neoverse-v1", "max"}


def _fields(value: Any, allowed: set[str], required: set[str], where: str) -> dict:
    if not isinstance(value, dict):
        raise TopologyError(f"{where} must be an object")
    extra = set(value) - allowed
    missing = required - set(value)
    if extra or missing:
        raise TopologyError(f"{where}: unknown fields {sorted(extra)}; missing fields {sorted(missing)}")
    return value


def _list(value: Any, where: str) -> list:
    if not isinstance(value, list) or not value:
        raise TopologyError(f"{where} must be a non-empty array")
    return value


def _integer(value: Any, where: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise TopologyError(f"{where} must be an integer >= {minimum}")
    return value


def _name(value: Any, where: str) -> str:
    if not isinstance(value, str) or not value or any(c not in
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in value):
        raise TopologyError(f"{where} must use letters, digits, '_' or '-'")
    return value


def _choice(value: Any, choices: set[str], where: str) -> str:
    if not isinstance(value, str) or value not in choices:
        raise TopologyError(f"{where} must be one of {', '.join(sorted(choices))}")
    return value


def compile_topology(raw: Any) -> dict[str, Any]:
    spec = _fields(raw, {"schema_version", "name", "nodes", "devices", "switches",
                         "links", "execution", "machine"},
                   {"schema_version", "name", "nodes", "devices", "switches",
                    "links", "execution"}, "topology")
    if spec["schema_version"] != 1 or type(spec["schema_version"]) is not int:
        raise TopologyError("only schema_version 1 is supported")
    _name(spec["name"], "name")
    nodes = _list(spec["nodes"], "nodes")
    devices = _list(spec["devices"], "devices")
    switches = spec["switches"]
    if not isinstance(switches, list):
        raise TopologyError("switches must be an array")
    links = _list(spec["links"], "links")
    execution = _fields(spec["execution"], {"sync", "checkpoint"},
                        {"sync", "checkpoint"}, "execution")
    _choice(execution["sync"], {"off", "required"}, "execution.sync")
    _choice(execution["checkpoint"], {"none", "auto"}, "execution.checkpoint")

    node_names: list[str] = []
    simulators: set[str] = set()
    profiles: set[str] = set()
    qemu_nodes: list[dict[str, Any]] = []
    for index, raw_node in enumerate(nodes):
        node = _fields(raw_node, {"id", "simulator", "profile", "cpu", "cores", "memory_mib"},
                       {"id", "simulator"}, f"nodes[{index}]")
        name = _name(node["id"], f"nodes[{index}].id")
        if name in node_names:
            raise TopologyError(f"duplicate node id {name}")
        node_names.append(name)
        _choice(node["simulator"], {"gem5", "qemu"}, f"{name}.simulator")
        simulators.add(node["simulator"])
        if node["simulator"] == "gem5":
            if any(field in node for field in ("cpu", "cores", "memory_mib")):
                raise TopologyError(f"{name}: gem5 per-node CPU and memory settings are not supported yet")
            _choice(node.get("profile"), {"fast", "kvm", "server", "modern", "udma400"},
                    f"{name}.profile")
            profiles.add(node["profile"])
        else:
            if "profile" in node:
                raise TopologyError(f"{name}: QEMU uses cpu, cores and memory_mib, not profile")
            cpu = _choice(node.get("cpu", "cortex-a72"), QEMU_CPUS, f"{name}.cpu")
            cores = _integer(node.get("cores", 1), f"{name}.cores", 1)
            memory_mib = _integer(node.get("memory_mib", 1024), f"{name}.memory_mib", 512)
            if cores > 16 or memory_mib > 8192:
                raise TopologyError(f"{name}: QEMU limit is 16 cores and 8192 MiB per node")
            qemu_nodes.append({"cpu": cpu, "cores": cores, "memory_mib": memory_mib})
    if len(simulators) != 1:
        raise TopologyError("mixed gem5/QEMU nodes need a new synchronized launcher")
    simulator = simulators.pop()
    if simulator == "gem5" and not 2 <= len(nodes) <= 8:
        raise TopologyError("current gem5 launcher supports 2..8 nodes")
    if simulator == "qemu" and not 2 <= len(nodes) <= 8:
        raise TopologyError("current QEMU launcher supports 2..8 nodes")
    if len(profiles) > 1:
        raise TopologyError("current gem5 launcher requires one shared machine profile")
    if simulator == "qemu" and execution["checkpoint"] != "none":
        raise TopologyError("current QEMU launcher does not support checkpoint restore")
    machine: dict[str, Any] = {}
    if "machine" in spec:
        if simulator != "gem5":
            raise TopologyError("machine CPU tuning is currently supported only for gem5")
        machine = _fields(spec["machine"],
                          {"o3_lq_entries", "o3_sq_entries", "l1d_prefetcher",
                           "memory_size_gb"},
                          set(), "machine")
        if "memory_size_gb" in machine:
            memory_size_gb = _integer(machine["memory_size_gb"],
                                      "machine.memory_size_gb", 4)
            if memory_size_gb > 128:
                raise TopologyError("machine.memory_size_gb exceeds 128")
        for field in ("o3_lq_entries", "o3_sq_entries"):
            if field in machine:
                _integer(machine[field], f"machine.{field}", 1)
                if machine[field] > 1024:
                    raise TopologyError(f"machine.{field} exceeds gem5 launcher limit")
        if "l1d_prefetcher" in machine:
            _choice(machine["l1d_prefetcher"],
                    {"none", "stride", "stride_exclusive", "stride_exclusive_d4",
                     "stride_exclusive_xpage"}, "machine.l1d_prefetcher")

    if len(switches) > 1:
        raise TopologyError("current ns-3 adapter supports at most one switch")
    direct = not switches
    switch_name = None
    switch_delay = 0
    if not direct:
        switch = _fields(switches[0], {"id", "type", "delay_ns"},
                         {"id", "type", "delay_ns"}, "switches[0]")
        switch_name = _name(switch["id"], "switches[0].id")
        if switch["type"] != "ns3ub":
            raise TopologyError("switches[0].type must be ns3ub")
        switch_delay = _integer(switch["delay_ns"], "switches[0].delay_ns")

    by_node: dict[str, dict] = {}
    by_device: dict[str, dict] = {}
    counts: set[int] = set()
    limits: set[int] = set()
    specified_limits = 0
    for index, raw_device in enumerate(devices):
        device = _fields(raw_device, {"id", "node", "type", "eid", "ports", "max_jettys"},
                         {"id", "node", "type", "eid", "ports"}, f"devices[{index}]")
        name = _name(device["id"], f"devices[{index}].id")
        if name in by_device or name == switch_name:
            raise TopologyError(f"duplicate component id {name}")
        if device["node"] not in node_names or device["node"] in by_node:
            raise TopologyError(f"{name}: node must exist and have exactly one UDMA device")
        if device["type"] != "udma":
            raise TopologyError(f"{name}: only UDMA devices are supported")
        node_index = node_names.index(device["node"])
        if _integer(device["eid"], f"{name}.eid") != 0x100 + node_index:
            raise TopologyError(f"{name}: current launcher requires EID {0x100 + node_index}")
        count = _integer(device["ports"], f"{name}.ports", 1)
        if count > (16 if simulator == "gem5" else 8):
            raise TopologyError(f"{name}: port count exceeds current {simulator} limit")
        counts.add(count)
        if "max_jettys" in device:
            specified_limits += 1
            limit = _integer(device["max_jettys"], f"{name}.max_jettys", 2)
            if limit > 65535:
                raise TopologyError(f"{name}.max_jettys exceeds the device limit 65535")
            limits.add(limit)
        by_node[device["node"]] = device
        by_device[name] = device
    if len(by_node) != len(nodes) or len(devices) != len(nodes):
        raise TopologyError("current launcher requires exactly one UDMA device per node")
    if len(counts) != 1 or len(limits) > 1 or specified_limits not in (0, len(devices)):
        raise TopologyError("current launcher requires equal port and Jetty limits on all devices")
    port_count = counts.pop()
    if direct:
        if len(nodes) != 2 or port_count != 1 or len(links) != 1:
            raise TopologyError("direct UB link requires two nodes, one port each and one link")
        link = _fields(links[0], {"device", "device_port", "peer_device", "peer_port",
                                  "rate_gbps", "latency_ns"},
                       {"device", "device_port", "peer_device", "peer_port",
                        "rate_gbps", "latency_ns"}, "links[0]")
        if {link["device"], link["peer_device"]} != set(by_device):
            raise TopologyError("direct UB link must connect the two distinct UDMA devices")
        if (_integer(link["device_port"], "links[0].device_port") != 0 or
                _integer(link["peer_port"], "links[0].peer_port") != 0):
            raise TopologyError("direct UB link must use port 0 on both devices")
        rate = _integer(link["rate_gbps"], "links[0].rate_gbps", 1)
        latency = _integer(link["latency_ns"], "links[0].latency_ns", 2)
        if rate > 100000 or latency > 1000000000:
            raise TopologyError("direct UB link rate or delay exceeds launcher limit")
        node_links = [(rate, latency // 2), (rate, latency - latency // 2)]
    else:
        if len(links) != len(nodes) * port_count:
            raise TopologyError("every device port needs one dedicated switch link")
        seen_device_ports: set[tuple[str, int]] = set()
        seen_switch_ports: set[int] = set()
        link_by_device: dict[str, tuple[int, int]] = {}
        for index, raw_link in enumerate(links):
            where = f"links[{index}]"
            link = _fields(raw_link, {"device", "device_port", "switch", "switch_port",
                                      "rate_gbps", "latency_ns"},
                           {"device", "device_port", "switch", "switch_port",
                            "rate_gbps", "latency_ns"}, where)
            if link["device"] not in by_device or link["switch"] != switch_name:
                raise TopologyError(f"{where}: unknown device or switch")
            device_port = _integer(link["device_port"], f"{where}.device_port")
            switch_port = _integer(link["switch_port"], f"{where}.switch_port")
            if device_port >= port_count:
                raise TopologyError(f"{where}: device port out of range")
            device = by_device[link["device"]]
            required_port = node_names.index(device["node"]) * port_count + device_port
            if switch_port != required_port:
                raise TopologyError(f"{where}: current adapter maps this port to switch port {required_port}")
            endpoint = (link["device"], device_port)
            if endpoint in seen_device_ports or switch_port in seen_switch_ports:
                raise TopologyError(f"{where}: port is connected more than once")
            seen_device_ports.add(endpoint)
            seen_switch_ports.add(switch_port)
            rate = _integer(link["rate_gbps"], f"{where}.rate_gbps", 1)
            latency = _integer(link["latency_ns"], f"{where}.latency_ns", 1)
            if rate > 100000 or latency > 1000000000:
                raise TopologyError(f"{where}: rate or propagation delay exceeds launcher limit")
            previous = link_by_device.setdefault(link["device"], (rate, latency))
            if previous != (rate, latency):
                raise TopologyError(f"{where}: all ports of one device currently need the same rate and delay")
        node_links = [link_by_device[by_node[node]["id"]] for node in node_names]
    return {"name": spec["name"], "simulator": simulator,
            "node_ids": node_names, "device_ids": [by_node[n]["id"] for n in node_names],
            "switch_id": switch_name, "fabric_layout": "direct" if direct else "switch",
            "backend": "native-json", "port_count": port_count,
            "max_jettys": next(iter(limits)) if limits else
            (47 if simulator == "qemu" or "modern" in profiles else 1152),
            "jetty_limit_explicit": bool(limits),
            "switch_delay_ns": switch_delay,
            "sync": execution["sync"], "checkpoint": execution["checkpoint"],
            "node_links": [{"rate_gbps": rate, "latency_ns": delay}
                           for rate, delay in node_links],
            "machines": qemu_nodes if simulator == "qemu" else
            [{"profile": node["profile"], **machine} for node in nodes]}


def run(args: Sequence[str], runtime: str, root: Path) -> int:
    if len(args) != 2 or args[0] not in {"validate", "plan", "start"}:
        print("usage: ./lab topology validate|plan|start FILE.json", file=sys.stderr)
        return 2
    action, filename = args
    try:
        path = Path(filename)
        raw = json.loads(path.read_text(encoding="utf-8"))
        plan = compile_topology(raw)
    except (OSError, json.JSONDecodeError, TopologyError) as exc:
        print(f"lab topology: {exc}", file=sys.stderr)
        return 2
    if action == "validate":
        print(f"Valid topology '{plan['name']}': {len(plan['node_ids'])} {plan['simulator']} nodes, "
              f"{len(plan['device_ids'])} UDMA devices, "
              f"{'one ns-3 UB switch' if plan['switch_id'] else 'one direct UB link'}")
        return 0
    public_command = ["./lab", "topology", "start", filename]
    if action == "plan":
        print(json.dumps({**plan, "launch": public_command}, indent=2))
        return 0
    from .config_launcher import LaunchError, start_gem5, start_qemu
    try:
        output = (start_gem5(raw, plan) if plan["simulator"] == "gem5" else
                  start_qemu(raw, plan))
        (output / "topology.json").write_text(json.dumps(raw, indent=2) + "\n")
        (output / "topology-plan.json").write_text(
            json.dumps({**plan, "launch": ["./lab", "topology", "start", filename]},
                       indent=2) + "\n")
    except (LaunchError, OSError, ValueError) as exc:
        print(f"lab topology: {exc}", file=sys.stderr)
        return 2
    return 0
