#!/usr/bin/env python3
"""Diagnostic symmetric READ load; source failures are outcomes, not successes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("adapter")
p.add_argument("peer")
p.add_argument("output", type=Path)
p.add_argument("--count", type=int, required=True)
p.add_argument("--allow-workload-errors", action="store_true",
               help="diagnostic-only: permit source errors, never protocol or deadline failures")
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
binary_sha256 = {name: hashlib.sha256(Path(path).read_bytes()).hexdigest()
                 for name, path in (("peer", a.peer), ("adapter", a.adapter))}
processes, logs = [], []
start = time.monotonic()

def launch(name, command):
    log = (a.output / (name + ".log")).open("w")
    logs.append(log)
    proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
    processes.append(proc)
    return proc

try:
    peers = [launch(role, [a.peer, role, str(a.output / (name + ".sock")),
              str(a.output / (name + ".shm")), "required", f"ctp-read-burst-{a.count}"])
             for role, name in (("sender", "a"), ("receiver", "b"))]
    deadline = time.monotonic() + 5
    while not all((a.output / (n + ".sock")).exists() for n in ("a", "b")):
        if time.monotonic() > deadline:
            raise RuntimeError("socket deadline")
        time.sleep(.01)
    adapter = launch("adapter", [a.adapter, "--sync", "required", "--rate-gbps", "400",
        "--ports", "2", "--link-delay-ps", "100000", "--switch-delay-ps", "50000",
        "--ctp-retransmission", "on", "--ctp-rto-ps", "25600000",
        "--ctp-max-retransmissions", "7", "--endpoint", str(a.output / "a.sock") + ",0x101",
        "--endpoint", str(a.output / "b.sock") + ",0x202"])
    deadline = time.monotonic() + 35
    while any(peer.poll() is None for peer in peers) and adapter.poll() is None:
        if time.monotonic() > deadline:
            break
        time.sleep(.02)
    result = dict(count=a.count, allow_workload_errors=a.allow_workload_errors,
                  peer_rc=[x.poll() for x in peers], adapter_rc=adapter.poll(),
                  wall_seconds=time.monotonic()-start)
finally:
    for proc in processes:
        if proc.poll() is None:
            proc.terminate()
    for proc in processes:
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill(); proc.wait()
    for log in logs:
        log.close()
text = (a.output / "adapter.log").read_text()
stats = [line for line in text.splitlines() if line.startswith("[NS3_UB_NET_STATS]")]
result["stats"] = dict((k, int(v)) for k, v in re.findall(r"(\w+)=(\d+)(?=\s|$)", stats[-1])) if stats else {}
result["final_process_rc"] = [proc.returncode for proc in processes]
progress = [line for line in text.splitlines() if line.startswith("[NS3_UB_NET_PROGRESS]")]
result["work_drained"] = None
if progress:
    fields = dict((k, int(v)) for k, v in re.findall(r"(\w+)=(\d+)(?=\s|$)", progress[-1]))
    result["work_drained"] = (all(fields.get(k) == 0 for k in
        ("native_tasks", "pending_target_dma", "scheduled_packets")) and fields.get("outputs_empty") == 1)
result["peers"] = [(a.output / (name + ".log")).read_text() for name in ("sender", "receiver")]
result["binary_sha256"] = binary_sha256
result["peer_binary"] = str(Path(a.peer).resolve())
result["adapter_binary"] = str(Path(a.adapter).resolve())
peer_metrics = []
for raw in result["peers"]:
    lines = [line for line in raw.splitlines() if line.startswith("BURST_RESULT ")]
    peer_metrics.append(dict((k, int(v)) for k, v in re.findall(r"(\w+)=(\d+)(?=\s|$)", lines[0]))
                        if len(lines) == 1 else {})
result["peer_metrics"] = peer_metrics
result["evidence_schema_valid"] = all(all(key in metrics for key in
    ("max_reply_dwell_ps", "outalloc_backpressure")) for metrics in peer_metrics)
result["bounded_protocol_completion"] = (result["evidence_schema_valid"] and
    result["peer_rc"] == [0, 0] and result["adapter_rc"] in (None, 0) and
    result["final_process_rc"] == [0, 0, 0])
result["workload_success"] = (result["bounded_protocol_completion"] and
    all(result["stats"].get(k) == v for k, v in dict(native_wqes_submitted=2*a.count,
        native_wqes_completed=2*a.count, native_wqes_failed=0, pending_target_dma=0,
        ctp_request_segments=32*a.count, ctp_native_completions=32*a.count).items()))
result["total_wall_seconds"] = time.monotonic() - start
(a.output / "report.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result), flush=True)
raise SystemExit(0 if (result["bounded_protocol_completion"] if a.allow_workload_errors
                      else result["workload_success"]) else 1)
