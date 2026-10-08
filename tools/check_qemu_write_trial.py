#!/usr/bin/env python3
"""Read-only, fail-closed audit of successful WRITE-only synchronized QEMU trials.

UMDK perftest prints MiB/s under its historical MB/sec heading. A -B row
already aggregates both directions; the exchanged endpoint rows are NOT additive.
This audits final work counters, not whole-system checkpoint consistency.
"""
import argparse
import json
import math
from pathlib import Path
import re
import shlex
import time


class AuditError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise AuditError(message)


def last_fields(text, marker):
    lines = [line for line in text.splitlines() if line.startswith(f"[{marker}] ")]
    require(lines, f"missing {marker}")
    return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)(?=\s|$)", lines[-1])}


def equal(fields, key, expected):
    require(key in fields and fields[key] == expected,
            f"{key}: expected {expected}, got {fields.get(key, 'MISSING')}")


def command_config(command):
    args = shlex.split(command)
    require("urma_perftest" in args and "write_bw" in args and "-B" in args,
            "only explicit-size bidirectional write_bw is supported")
    config = {}
    for flag in ("-s", "-n", "-T", "-J", "-l", "-Q", "-I"):
        require(args.count(flag) == 1, f"missing or duplicate {flag}")
        try:
            value = int(args[args.index(flag) + 1])
        except (IndexError, ValueError) as error:
            raise AuditError(f"invalid {flag}") from error
        require(value >= 0 if flag == "-I" else value > 0, f"invalid range for {flag}")
        config[flag] = value
    require(config["-J"] == 1, "multi-Jetty iteration accounting not supported")
    require(not any(arg.startswith("-a") for arg in args), "scan commands not supported")
    return config


def audit(directory):
    directory = Path(directory)
    def read(name):
        try:
            return (directory / name).read_text()
        except OSError as error:
            raise AuditError(f"missing/unreadable evidence: {name}") from error
    report = json.loads(read("report.json"))
    manifest = dict(line.split("=", 1) for line in read("run-manifest.txt").splitlines() if "=" in line)
    for key, expected in {"runtime": "qemu-tcg-timing", "node_count": "2",
                          "network_backend": "modular-ns3ub", "synchronization": "required"}.items():
        require(manifest.get(key) == expected, f"manifest {key} must be {expected}")
    try:
        link_rate = float(manifest["peer_link_rate_gbps"])
    except (KeyError, ValueError) as error:
        raise AuditError("missing/invalid configured link rate") from error
    require(math.isfinite(link_rate) and link_rate > 0, "invalid configured link rate")
    records = report.get("records", [])
    require(isinstance(records, list) and all(isinstance(r, dict) for r in records), "invalid report records")
    for label in ("launch", "shell-ready", "network", "stop"):
        matches = [r for r in records if r.get("label") == label]
        require(len(matches) == 1 and matches[0].get("returncode") == 0,
                f"missing/failed {label}")
    cases = []
    expected_wqes = expected_payload = expected_segments = 0
    seen = set()
    for record in records:
        label = record.get("label", "")
        command = record.get("command", [])
        require(isinstance(command, list), "invalid command record")
        is_perf = any("urma_perftest" in str(item) for item in command)
        if not label.startswith("write-"):
            require(not is_perf and not label.startswith("read-"), "mixed/non-WRITE workload unsupported")
            continue
        require(re.fullmatch(r"write-\d+", label) and label not in seen, "invalid/duplicate WRITE label")
        seen.add(label)
        require(record.get("returncode") == 0 and record.get("validation_errors") == [],
                f"{label}: missing/failed runner validation")
        require(command.count("--commands") == 1, f"{label}: missing paired commands")
        pair = command[command.index("--commands") + 1:]
        require(len(pair) == 2, "expected exactly two guest commands")
        config = command_config(pair[0])
        require(command_config(pair[1]) == config, "endpoint commands differ")
        size, count, depth = config["-s"], config["-n"], config["-T"]
        require(label == f"write-{size}", "label/command size mismatch")
        raw = read(label + ".txt")
        blocks = re.split(r"(?m)^--- UART (\d+) ---\s*$", raw)
        require(len(blocks) == 5, "expected exactly two UART blocks")
        values = []
        for index, expected_port in ((1, manifest.get("uart0")), (3, manifest.get("uart1"))):
            port, body = blocks[index:index + 2]
            require(port == expected_port, "UART/manifest mismatch")
            rc = re.findall(r"(?m)^__UBSIM_RC_" + re.escape(port) + r"_\d+__=(-?\d+)\s*$", body)
            require(rc == ["0"], "missing/failed/duplicate guest return code")
            require("URMA_WRITE Bidirectional" in body, "not a bidirectional WRITE result")
            require("BW peak[MB/sec]" in body and "BW average[MB/sec]" in body,
                    "unexpected perftest unit header; refusing unit inference")
            require(re.findall(r"JFS depth\s*:\s*(\d+)", body) == [str(depth)], "SQ depth mismatch")
            require(re.findall(r"Max inline size\s*:\s*(\d+)\s*\[B\]", body) == [str(config["-I"])],
                    "inline size mismatch or missing header")
            rows = re.findall(r"(?m)^\s*(\d+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*$", body)
            require(len(rows) == 1, "missing/ambiguous bandwidth row")
            row = rows[0]
            require((int(row[0]), int(row[1])) == (size, count), "size/iterations mismatch")
            peak, average = float(row[2]), float(row[3])
            require(math.isfinite(peak) and math.isfinite(average) and 0 < average <= peak,
                    "invalid bandwidth values")
            values.append((peak, average))
        require(values[0] == values[1], "exchanged endpoint bandwidth rows differ")
        peak, average = values[0]
        cases.append(dict(size_bytes=size, iterations_per_endpoint=count, configured_sq_depth=depth,
                          inline_size=config["-I"],
                          peak_MiB_s=peak, average_MiB_s=average,
                          average_GB_s=average * 1048576 / 1e9,
                          aggregation="perftest -B combined directions, counted once"))
        expected_wqes += count
        expected_payload += 2 * count * size
        expected_segments += 2 * count * ((size + 4095) // 4096)
    require(cases, "no supported WRITE cases")
    fabric = last_fields(read("ns3.log"), "NS3_UB_NET_STATS")
    fabric_progress = last_fields(read("ns3.log"), "NS3_UB_NET_PROGRESS")
    for key in ("native_tasks", "pending_target_dma", "scheduled_packets", "native_wqes_failed"):
        equal(fabric_progress, key, 0)
    equal(fabric_progress, "outputs_empty", 1)
    for key in ("native_wqes_submitted", "native_wqes_completed"):
        equal(fabric_progress, key, 2 * expected_wqes)
    for key in ("native_wqes_submitted", "native_wqes_completed"):
        equal(fabric, key, 2 * expected_wqes)
    equal(fabric, "payload_bytes", expected_payload)
    for key in ("ctp_request_segments", "ctp_taacks", "ctp_native_completions"):
        equal(fabric, key, expected_segments)
    for key in ("native_wqes_failed", "target_dma_errors", "pending_target_dma",
                "ctp_read_responses", "ns3_runtime_drops", "native_trace_size_mismatches",
                "ctp_retransmission_exhausted", "async_timestamp_jumps"):
        equal(fabric, key, 0)
    nodes = []
    for node in range(2):
        text = read(f"udma-node{node}-udma.log")
        profile = last_fields(text, "UDMA_PROFILE")
        progress = last_fields(text, "UDMA_PROGRESS")
        for fields in (profile, progress):
            for key in ("sq_wqes", "sq_completions"):
                equal(fields, key, expected_wqes)
        for key in ("sq_depth_rejects", "sq_decode_rejects", "ubase_errors", "async_timestamp_jumps"):
            equal(profile, key, 0)
        equal(progress, "quiescent", 1)
        for key in ("pending_rma", "read_dma_queues", "remote_services", "completion_events",
                    "event_busy", "receive_busy", "destroy_pending"):
            equal(progress, key, 0)
        host = last_fields(text, "UB_HOST_PENDING")
        equal(host, "reads", 0); equal(host, "writes", 0)
        for key in ("rma_pending_peak", "sq_consumer_wraps"):
            require(key in profile, f"missing {key}")
        nodes.append(dict(node=node, wqes=expected_wqes,
                          observed_rma_pending_peak=profile["rma_pending_peak"],
                          observed_sq_consumer_wraps=profile["sq_consumer_wraps"]))
    retry_keys = ("ctp_retransmissions", "duplicate_write_suppressed", "duplicate_taack_replays",
                  "native_late_response_received", "ctp_retries_queued", "ctp_retries_cancelled",
                  "ctp_retries_stale_sent")
    return dict(status="PASS", cases=cases, nodes=nodes, expected_fabric_wqes=2*expected_wqes,
                retry_diagnostics={key: fabric.get(key) for key in retry_keys},
                configured_link_rate_gbps=link_rate,
                scope="WRITE-only final work-counter audit; not a whole-system checkpoint",
                outstanding_note="UDMA RMA peak is observed, not configured SQ depth or CTP TA window",
                trial_wall_seconds=report.get("wall_seconds"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    begin = time.monotonic()
    try:
        result = audit(args.directory)
    except (AuditError, ValueError, TypeError, KeyError) as error:
        print(json.dumps(dict(status="FAIL", error=str(error))))
        return 1
    result["audit_wall_seconds"] = time.monotonic() - begin
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
