"""Resolve declarative topology configuration without invoking a shell launcher.

The profile table is a versioned copy of the formerly implicit launcher
defaults.  The topology schema owns all fields that users can edit in the
workbench; machine defaults remain explicit and inspectable here.
"""

from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROFILE_DEFAULTS = json.loads((Path(__file__).with_name("gem5_profile_defaults.json")).read_text())


def resolve_gem5(spec: dict, plan: dict) -> dict[str, str]:
    if plan["simulator"] != "gem5":
        raise ValueError("gem5 configuration requires gem5 topology")
    profile = spec["nodes"][0]["profile"]
    values = PROFILE_DEFAULTS[profile].copy()
    count = len(plan["node_ids"])
    links = plan["node_links"]
    latency = min(link["latency_ns"] for link in links)
    quantum = min(100, latency) if profile == "modern" else latency
    machine = spec.get("machine", {})
    memory = f"{machine.get('memory_size_gb', 32)}GB"
    initrd = ("out/official-udma-64k-node-template-20261021.cpio.gz"
              if profile == "modern" and memory == "4GB" else
              "out/official-udma-64k-20261015.cpio.gz")
    required_sync = spec["execution"]["sync"] == "required"
    lifecycle = required_sync
    values.update({
        "initrd_path": str(ROOT / initrd),
        "execution_mode": "native",
        "node_count": str(count),
        "memory_size": memory,
        "peer_latency_ns": str(latency),
        "sync_quantum_ns": str(quantum),
        "external_udma_host_latency_ns": str(quantum),
        "sync_request": "roi" if required_sync else "off",
        "virtual_time_synchronization": "lifecycle" if lifecycle else "disabled",
        "lifecycle_sync": "1" if lifecycle else "0",
        "restore_checkpoint": "none",
        "template_checkpoint": "0",
        "ub_fabric_layout": plan["fabric_layout"],
        "ub_port_count": str(plan["port_count"]),
        "peer_link_rate_gbps": str(links[0]["rate_gbps"]),
        "peer_switch_delay": f"{plan['switch_delay_ns']}ns",
        "max_jettys": str(plan["max_jettys"]),
        "public_jettys": "1" if plan["jetty_limit_explicit"] else values["public_jettys"],
    })
    if profile == "modern" and lifecycle:
        values.update(auto_o3_lifecycle="1", fast_boot_noncaching="1",
                      initial_memory_mode="atomic_noncaching",
                      cpu_switch_policy="perftest_lifecycle_enter_o3_exit_atomic")
    else:
        values.update(auto_o3_lifecycle="0", fast_boot_noncaching="0")
        if profile == "modern":
            values.update(initial_memory_mode="atomic",
                          cpu_switch_policy="send_lat_roi_enter_o3_exit_atomic")
    for key in ("o3_lq_entries", "o3_sq_entries"):
        if key in machine:
            values[key] = str(machine[key])
    if "l1d_prefetcher" in machine:
        values["cache_prefetcher"] = machine["l1d_prefetcher"]
    for index, link in enumerate(links):
        values[f"node{index}_link_latency_ns"] = str(link["latency_ns"])
        values[f"node{index}_link_rate_gbps"] = str(link["rate_gbps"])
    for index in range(count, 8):
        values.pop(f"node{index}_link_latency_ns", None)
        values.pop(f"node{index}_link_rate_gbps", None)
    return values
