#!/usr/bin/env python3
"""Bounded late-DMA contract; an unfixed adapter is expected to fail this test."""
import argparse
import pathlib
import re
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("adapter")
p.add_argument("peer")
p.add_argument("work", type=pathlib.Path)
p.add_argument("--profile", choices=("ctp-late-dma", "ctp-late-read", "ctp-late-dma-bad-length", "ctp-late-first"),
               default="ctp-late-dma")
a = p.parse_args()
a.work.mkdir(parents=True, exist_ok=False)
processes = []
logs = []
start = time.monotonic()

def launch(name, args):
    log = (a.work / (name + ".log")).open("w")
    logs.append(log)
    proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
    processes.append(proc)
    return proc

try:
    peers = [launch(role, [a.peer, role, str(a.work / (name + ".sock")),
                          str(a.work / (name + ".shm")), "required", a.profile])
             for role, name in (("sender", "a"), ("receiver", "b"))]
    deadline = time.monotonic() + 5
    while not all((a.work / (n + ".sock")).exists() for n in ("a", "b")):
        if time.monotonic() > deadline:
            raise RuntimeError("peer socket deadline")
        time.sleep(0.01)
    # Native switch processing, not IPC link delay, places first target
    # execution after the 204.8us sender retry budget.
    extra = ["--switch-delay-ps", "300000000"] if a.profile == "ctp-late-first" else []
    adapter = launch("adapter", [a.adapter, "--sync", "required",
                     "--ctp-retransmission", "on", "--ctp-rto-ps", "25600000",
                     "--ctp-max-retransmissions", "7",
                     "--endpoint", str(a.work / "a.sock") + ",0x101",
                     "--endpoint", str(a.work / "b.sock") + ",0x202"] + extra)
    deadline = time.monotonic() + 20
    while any(peer.poll() is None for peer in peers):
        if adapter.poll() is not None or time.monotonic() > deadline:
            break
        time.sleep(0.02)
    valid = all(peer.poll() == 0 for peer in peers) and adapter.poll() in (None, 0)
    if a.profile == "ctp-late-dma-bad-length":
        # A failure is expected, but not an arbitrary crash or unrelated error.
        adapter_text = (a.work / "adapter.log").read_text()
        valid = adapter.poll() == 1 and "mismatched external target DMA completion" in adapter_text
    summary = {"peer_rc": [peer.poll() for peer in peers],
               "adapter_rc": adapter.poll(), "wall_seconds": time.monotonic() - start}
finally:
    for proc in processes:
        if proc.poll() is None:
            proc.terminate()
    for proc in processes:
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    for log in logs:
        log.close()
if a.profile != "ctp-late-dma-bad-length":
    text = (a.work / "adapter.log").read_text()
    expected = {"native_wqes_submitted": 1, "native_wqes_completed": 0,
                "native_wqes_failed": 1, "target_dma_drained_after_terminal": 1,
                "pending_target_dma": 0}
    if a.profile == "ctp-late-first":
        expected["native_target_after_terminal"] = 1
    if a.profile != "ctp-late-first":
        expected["native_late_response_received"] = 1
    stats = [line for line in text.splitlines() if line.startswith("[NS3_UB_NET_STATS]")]
    valid &= len(stats) == 1 and all(
        re.search(r"(?:^|\s)" + key + "=" + str(value) + r"(?:\s|$)", stats[0])
        for key, value in expected.items())
    if a.profile == "ctp-late-first":
        late = re.search(r"\bnative_late_response_received=(\d+)\b", stats[0]) if stats else None
        valid &= late is not None and int(late[1]) >= 1
print(dict(summary, accepted=bool(valid)), flush=True)
raise SystemExit(0 if valid else 1)
