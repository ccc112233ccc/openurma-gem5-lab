#!/usr/bin/env python3
"""Read-only, fail-closed audit of bidirectional WRITE/READ/SEND or one-way READ.

UMDK perftest prints MiB/s under its historical MB/sec heading. A -B row
already aggregates both directions; the exchanged endpoint rows are NOT additive.
Without -B, only the READ client posts work; the server prints its exchanged row.
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


def command_config(command, role=None):
    args = shlex.split(command)
    verbs = [verb for verb in ("write", "read", "send") if f"{verb}_bw" in args]
    require("urma_perftest" in args and len(verbs) == 1,
            "only explicit-size write/read/send_bw is supported")
    require(args.count("-B") <= 1, "duplicate bidirectional flag")
    config = {"verb": verbs[0], "bidirectional": "-B" in args}
    require(config["bidirectional"] or config["verb"] == "read",
            "only READ supports unidirectional auditing")
    # Only modes whose reporting semantics are modeled below are accepted.
    executable = args.index("urma_perftest")
    require(all(token.startswith("LD_LIBRARY_PATH=") or token == "UBSIM_BW_REPORT_DIAGNOSTIC=1"
                for token in args[:executable]), "unsupported command prefix")
    remaining = args[executable + 2:]
    value_flags = {"-s", "-n", "-T", "-J", "-l", "-Q", "-I", "-R", "-d", "--eid_idx", "-S", "-P", "-p", "-C"}
    while remaining:
        flag = remaining[0]
        if flag in ("-B", "--ctp"):
            remaining = remaining[1:]
        else:
            require(flag in value_flags and len(remaining) >= 2, f"unsupported command mode/option: {flag}")
            remaining = remaining[2:]
    require(args.count("--ctp") == 1, "explicit CTP transport required")
    for flag, expected in (("-d", "udma0"), ("--eid_idx", "0")):
        require(args.count(flag) == 1 and args.index(flag) + 1 < len(args) and
                args[args.index(flag) + 1] == expected, f"expected {flag} {expected}")
    if role == "server":
        require("-S" not in args, "server must not specify client destination")
    elif role == "client":
        require(args.count("-S") == 1 and args.index("-S") + 1 < len(args) and
                args[args.index("-S") + 1] == "10.0.0.1", "client must connect to 10.0.0.1")
    flags = ["-s", "-n", "-T", "-J", "-l", "-Q", "-I"]
    if config["verb"] in ("read", "send") or "-R" in args:
        flags.append("-R")
    for flag in flags:
        require(args.count(flag) == 1, f"missing or duplicate {flag}")
        try:
            value = int(args[args.index(flag) + 1])
        except (IndexError, ValueError) as error:
            raise AuditError(f"invalid {flag}") from error
        require(value >= 0 if flag == "-I" else value > 0, f"invalid range for {flag}")
        config[flag] = value
    require(config["-J"] == 1, "multi-Jetty iteration accounting not supported")
    require(config["verb"] != "read" or config["-I"] == 0,
            "READ does not support inline; require explicit -I 0")
    require(config["verb"] != "send" or config["-s"] <= 4096,
            "SEND sizes above 4096 are outside the supported trial contract")
    require(not any(arg.startswith("-a") for arg in args), "scan commands not supported")
    return config


def peak_policy(verb, iterations):
    if iterations > 20000:
        return [1, 1], "disabled on both endpoints (iterations > 20000)"
    if verb == "send":
        return [1, 0], "client-only peak; average and message rate combine both directions"
    return [0, 0], "sum of both endpoint peaks; average combines both directions"


def check_report_diagnostic(line, config, no_peak):
    fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
    require(fields.get("observation") == "existing_samples_only", "unknown diagnostic observation scope")
    def number(key):
        require(key in fields, f"missing report diagnostic {key}")
        value = float(fields[key])
        require(math.isfinite(value), f"nonfinite report diagnostic {key}")
        return value
    def matches(key, expected):
        require(math.isclose(number(key), expected, rel_tol=1e-12, abs_tol=1e-9), f"diagnostic arithmetic mismatch: {key}")
    for key, expected in {"api_type": {"read": 0, "write": 1, "send": 2}[config["verb"]],
                          "bidirection": 1, "no_peak": no_peak, "jettys": 1,
                          "size": config["-s"], "iters_sum": config["-n"], "cfg_iters": config["-n"],
                          "post_list": config["-l"], "cq_mod": config["-Q"],
                          "completed_index": 0 if no_peak else config["-n"]-1}.items():
        matches(key, expected)
    scale = number("scale")
    require(scale > 0, "diagnostic scale must be positive")
    matches("scale", number("cpu_mhz") * 1e6)
    cycles = number("tcompleted_last") - number("tposted0")
    require(cycles > 0 and number("opt_delta") > 0, "invalid diagnostic cycle interval")
    matches("cycles_sum", cycles)
    matches("array0", number("tposted0"))
    if no_peak:
        matches("tcompleted_last", number("tcompleted0"))
        matches("opt_delta", number("tcompleted0") - number("array0"))
    matches("peak_up", (1-no_peak)*config["-s"]*scale)
    matches("peak_down", number("opt_delta")*1048576)
    matches("avg_up", config["-s"]*config["-n"]*scale)
    matches("avg_down", cycles*1048576)
    matches("peak", number("peak_up")/number("peak_down"))
    matches("avg", number("avg_up")/number("avg_down"))
    if not no_peak:
        require(number("opt_delta") <= math.floor(cycles / config["-n"]),
                "enabled peak minimum excludes the full interval candidate")
    return number("peak"), number("avg"), config["-n"]*scale/cycles/1e6


def size_compliance(directory, manifest, cases):
    """Advertised size bounds are a separate claim from successful counters."""
    path = directory / "capabilities.txt"
    if not path.exists():
        return dict(advertised_size_compliance=None, advertised_max_msg_size_by_node=None,
                    capability_warnings=["Advertised size compliance unknown: capabilities.txt absent"])
    try:
        blocks = re.split(r"(?m)^--- UART (\d+) ---\s*$", path.read_text())
        require(len(blocks) == 5, "expected two capability UART blocks")
        limits = {}
        for node, index in enumerate((1, 3)):
            port, body = blocks[index:index + 2]
            require(port == manifest.get(f"uart{node}"), "capability UART mismatch")
            rc = re.findall(r"(?m)^__UBSIM_RC_" + re.escape(port) + r"_\d+__=(-?\d+)\s*$", body)
            require(rc == ["0"], "capability command missing successful return code")
            sizes = re.findall(r"(?m)^\s*max_msg_size\s*:\s*(\d+)\s*$", body)
            require(len(sizes) == 1 and int(sizes[0]) > 0, "missing/invalid max_msg_size")
            limits[f"node{node}"] = int(sizes[0])
    except (OSError, ValueError) as error:
        return dict(advertised_size_compliance=None, advertised_max_msg_size_by_node=None,
                    capability_warnings=[f"Advertised size compliance unknown: {error}"])
    excessive = [case["size_bytes"] for case in cases if case["size_bytes"] > min(limits.values())]
    return dict(advertised_size_compliance=not excessive, advertised_max_msg_size_by_node=limits,
                capability_warnings=([f"Sizes {excessive} exceed at least one advertised max_msg_size; "
                                      "counter success is NOT advertised hardware support"] if excessive else []))


def audit(directory, verb=None, expected_sizes=None):
    if expected_sizes is not None:
        require(isinstance(expected_sizes, (list, tuple)) and len(expected_sizes) > 0,
                "expected sizes must be a nonempty sequence")
        require(all(type(size) is int and 0 < size <= 0xffffffff for size in expected_sizes),
                "expected sizes must be positive uint32 integers")
        require(len(set(expected_sizes)) == len(expected_sizes), "duplicate expected sizes")
        expected_sizes = list(expected_sizes)
    require(verb in (None, "write", "read", "send"), "unsupported requested verb")
    directory = Path(directory)
    def read(name):
        try:
            return (directory / name).read_text()
        except OSError as error:
            raise AuditError(f"missing/unreadable evidence: {name}") from error
    report = json.loads(read("report.json"))
    manifest = dict(line.split("=", 1) for line in read("run-manifest.txt").splitlines() if "=" in line)
    retry_mode = manifest.get("ctp_retransmission")
    profiles = {"on": "experimental-ta-timeout-replay", "off": "ctp-lossless-lower-layer-assumed"}
    require(retry_mode is None or retry_mode in profiles, "invalid effective CTP retry mode")
    observed_profiles = re.findall(r"\breliability_profile=(\S+)", read("ns3.log"))
    if observed_profiles:
        require(retry_mode in profiles and observed_profiles == [profiles[retry_mode]],
                "runtime CTP profile disagrees with manifest")
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
    trial_verb = verb
    trial_bidirectional = None
    for record in records:
        label = record.get("label", "")
        command = record.get("command", [])
        require(isinstance(command, list), "invalid command record")
        is_perf = any("urma_perftest" in str(item) for item in command)
        if not re.match(r"^(write|read|send)-", label):
            require(not is_perf, "unsupported performance workload")
            continue
        require(re.fullmatch(r"(write|read|send)-\d+", label) and label not in seen, "invalid/duplicate workload label")
        seen.add(label)
        require(record.get("returncode") == 0 and record.get("validation_errors") == [],
                f"{label}: missing/failed runner validation")
        require(command.count("--commands") == 1, f"{label}: missing paired commands")
        pair = command[command.index("--commands") + 1:]
        require(len(pair) == 2, "expected exactly two guest commands")
        config = command_config(pair[0], "server")
        require(command_config(pair[1], "client") == config, "endpoint commands differ")
        if trial_verb is None:
            trial_verb = config["verb"]
        require(config["verb"] == trial_verb, "mixed verbs or requested verb mismatch")
        if trial_bidirectional is None:
            trial_bidirectional = config["bidirectional"]
        require(config["bidirectional"] == trial_bidirectional, "mixed traffic directions")
        directions = 2 if trial_bidirectional else 1
        size, count, depth = config["-s"], config["-n"], config["-T"]
        require(label == f"{trial_verb}-{size}", "label/command verb or size mismatch")
        raw = read(label + ".txt")
        blocks = re.split(r"(?m)^--- UART (\d+) ---\s*$", raw)
        require(len(blocks) == 5, "expected exactly two UART blocks")
        values = []
        diagnostics = []
        no_peak, peak_scope = peak_policy(trial_verb, count)
        if not trial_bidirectional:
            peak_scope = "client-only measurement, server displays exchanged client result"
        for index, expected_port in ((1, manifest.get("uart0")), (3, manifest.get("uart1"))):
            port, body = blocks[index:index + 2]
            require(port == expected_port, "UART/manifest mismatch")
            rc = re.findall(r"(?m)^__UBSIM_RC_" + re.escape(port) + r"_\d+__=(-?\d+)\s*$", body)
            require(rc == ["0"], "missing/failed/duplicate guest return code")
            require(f"URMA_{trial_verb.upper()}" in body and
                    ("Bidirectional" in body) == trial_bidirectional,
                    "wrong verb or direction result")
            require("BW peak[MB/sec]" in body and "BW average[MB/sec]" in body,
                    "unexpected perftest unit header; refusing unit inference")
            expected_depth = [] if not trial_bidirectional and index == 1 else [str(depth)]
            require(re.findall(r"JFS depth\s*:\s*(\d+)", body) == expected_depth, "SQ depth mismatch")
            require(re.findall(r"CQ Moderation\s*:\s*(\d+)", body) == [str(config["-Q"])],
                    "CQ moderation mismatch")
            require(re.findall(r"Number of jettys\s*:\s*(\d+)", body) == [str(config["-J"])],
                    "Jetty count mismatch")
            post_lists = re.findall(r"JFS post list\s*:\s*(\d+)", body)
            require(post_lists in ([], [str(config["-l"])]), "post list mismatch")
            inline_headers = re.findall(r"Max inline size\s*:\s*(\d+)\s*\[B\]", body)
            if trial_verb == "read":
                # Official perftest omits this line for READ/ATOMIC. The
                # explicit -I0 command still guards normalization ambiguity.
                require(inline_headers in ([], ["0"]), "unexpected READ inline header")
            else:
                require(inline_headers == [str(config["-I"])], "inline size mismatch or missing header")
            rows = re.findall(r"(?m)^\s*(\d+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*$", body)
            require(len(rows) == 1, "missing/ambiguous bandwidth row")
            row = rows[0]
            require((int(row[0]), int(row[1])) == (size, count), "size/iterations mismatch")
            peak, average, message_rate = float(row[2]), float(row[3]), float(row[4])
            require(math.isfinite(peak) and math.isfinite(average) and average > 0 and peak >= 0 and
                    math.isfinite(message_rate) and message_rate >= 0,
                    "invalid bandwidth values")
            if no_peak == [0, 0]:
                require(average <= peak, "peak-enabled aggregate average exceeds peak")
            elif no_peak == [1, 1]:
                require(peak == 0, "peak must be zero when both endpoints disable peak")
            else:
                require(peak > 0, "client-only enabled SEND peak must be positive")
            values.append((peak, average, message_rate))
            lines = [line for line in body.splitlines() if line.startswith("[UBSIM_BW_REPORT_DIAGNOSTIC]")]
            require(trial_bidirectional or not lines, "unidirectional report diagnostics not supported")
            require(len(lines) <= 1, "duplicate report diagnostics")
            diagnostics.append(check_report_diagnostic(lines[0], config, no_peak[len(values)-1]) if lines else None)
        require(values[0] == values[1], "exchanged endpoint bandwidth rows differ")
        peak, average, message_rate = values[0]
        if any(item is not None for item in diagnostics):
            require(all(item is not None for item in diagnostics), "diagnostics missing on one endpoint")
            sums = [sum(item[column] for item in diagnostics) for column in range(3)]
            require(all(abs(value-total) <= precision for value,total,precision in
                        zip(values[0], sums, (0.005001, 0.005001, 0.000000501))),
                    "summed local diagnostics disagree with printed aggregate rounding")
        cases.append(dict(verb=trial_verb, bidirectional=trial_bidirectional, size_bytes=size,
                          iterations_per_endpoint=count if trial_bidirectional else None,
                          iterations_by_node=[count, count] if trial_bidirectional else [0, count],
                          configured_sq_depth=depth,
                          configured_receive_depth=config.get("-R"),
                          inline_size=config["-I"],
                          peak_MiB_s=peak, average_MiB_s=average,
                          message_rate_Mpps=message_rate,
                          peak_scope=peak_scope, endpoint_no_peak=no_peak if trial_bidirectional else [None, no_peak[1]],
                          report_diagnostics_checked=diagnostics[0] is not None,
                          average_GB_s=average * 1048576 / 1e9,
                          aggregation=("perftest -B combined directions, counted once" if trial_bidirectional
                                       else "client READ only; exchanged server row counted once")))
        expected_wqes += count
        expected_payload += directions * count * size
        expected_segments += directions * count * ((size + 4095) // 4096)
    require(cases, "no supported bandwidth cases")
    if expected_sizes is not None:
        require([case["size_bytes"] for case in cases] == expected_sizes,
                "completed size sequence does not match expected sizes")
    fabric = last_fields(read("ns3.log"), "NS3_UB_NET_STATS")
    fabric_progress = last_fields(read("ns3.log"), "NS3_UB_NET_PROGRESS")
    for key in ("native_tasks", "pending_target_dma", "scheduled_packets", "native_wqes_failed"):
        equal(fabric_progress, key, 0)
    equal(fabric_progress, "outputs_empty", 1)
    for key in ("native_wqes_submitted", "native_wqes_completed"):
        equal(fabric_progress, key, directions * expected_wqes)
    for key in ("native_wqes_submitted", "native_wqes_completed"):
        equal(fabric, key, directions * expected_wqes)
    # Adapter currently counts READ logical request bytes AND response bytes;
    # this diagnostic is not on-wire payload and must not double bandwidth.
    equal(fabric, "payload_bytes", expected_payload * (2 if trial_verb == "read" else 1))
    for key in ("ctp_request_segments", "ctp_native_completions"):
        equal(fabric, key, expected_segments)
    equal(fabric, "ctp_taacks", expected_segments if trial_verb == "write" else 0)
    equal(fabric, "ctp_read_responses", expected_segments if trial_verb == "read" else 0)
    if trial_verb == "send":
        equal(fabric, "ctp_native_send_delivery_completions", expected_segments)
    elif trial_verb == "read" or "ctp_native_send_delivery_completions" in fabric:
        equal(fabric, "ctp_native_send_delivery_completions", 0)
    for key in ("native_wqes_failed", "target_dma_errors", "pending_target_dma",
                "ns3_runtime_drops", "native_trace_size_mismatches",
                "ctp_retransmission_exhausted", "async_timestamp_jumps"):
        equal(fabric, key, 0)
    nodes = []
    for node in range(2):
        node_wqes = expected_wqes if trial_bidirectional or node == 1 else 0
        text = read(f"udma-node{node}-udma.log")
        profile = last_fields(text, "UDMA_PROFILE")
        progress = last_fields(text, "UDMA_PROGRESS")
        for fields in (profile, progress):
            for key in ("sq_wqes", "sq_completions"):
                equal(fields, key, node_wqes)
        for key in ("sq_depth_rejects", "sq_decode_rejects", "ubase_errors", "async_timestamp_jumps"):
            equal(profile, key, 0)
        equal(progress, "quiescent", 1)
        for key in ("pending_rma", "read_dma_queues", "remote_services", "completion_events",
                    "event_busy", "receive_busy", "destroy_pending"):
            equal(progress, key, 0)
        host = last_fields(text, "UB_HOST_PENDING")
        equal(host, "reads", 0); equal(host, "writes", 0)
        for key in (("rma_pending_peak", "sq_consumer_wraps") if trial_verb != "send" else ("sq_consumer_wraps",)):
            require(key in profile, f"missing {key}")
        node_result = dict(node=node, wqes=node_wqes,
                           observed_sq_consumer_wraps=profile["sq_consumer_wraps"])
        if trial_verb != "send":
            node_result["observed_rma_pending_peak"] = profile["rma_pending_peak"]
        else:
            node_result["send_outstanding_peak"] = None
            node_result["send_outstanding_note"] = "Not measured by rma_pending_peak; do not infer from SQ depth or TA window"
        nodes.append(node_result)
    retry_keys = ("ctp_retransmissions", "duplicate_write_suppressed", "duplicate_taack_replays",
                  "duplicate_read_suppressed", "duplicate_read_response_replays", "ctp_retries_active_sent",
                  "native_late_response_received", "ctp_retries_queued", "ctp_retries_cancelled",
                  "ctp_retries_stale_sent")
    compliance = size_compliance(directory, manifest, cases)
    return dict(status="PASS", verb=trial_verb, bidirectional=trial_bidirectional,
                cases=cases, nodes=nodes, expected_fabric_wqes=directions*expected_wqes,
                expected_sizes=expected_sizes,
                **compliance,
                expected_application_bytes=expected_payload,
                legacy_native_payload_bytes=fabric["payload_bytes"],
                native_payload_note="Legacy logical counter, NOT wire bytes or application payload; never used to calculate bandwidth",
                retry_diagnostics={key: fabric.get(key) for key in retry_keys},
                configured_link_rate_gbps=link_rate,
                ctp_retransmission=retry_mode,
                reliability_profile=profiles.get(retry_mode),
                runtime_reliability_profile_checked=bool(observed_profiles),
                scope=f"{trial_verb.upper()} counter/configuration consistency only; PASS is not full hardware support, two-sample drain proof or a whole-system checkpoint",
                fabric_payload_accounting="logical request plus response bytes" if trial_verb == "read" else "logical request bytes",
                outstanding_note=("SEND outstanding is not measured by RMA or CTP TA counters" if trial_verb == "send"
                                  else "UDMA RMA peak is observed, not configured SQ depth or CTP TA window"),
                trial_wall_seconds=report.get("wall_seconds"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--verb", choices=("write", "read", "send"), help="require this verb; otherwise detect it")
    parser.add_argument("--expected-sizes", type=int, nargs="+",
                        help="require exactly this completed size sequence, including order")
    args = parser.parse_args()
    begin = time.monotonic()
    try:
        result = audit(args.directory, args.verb, args.expected_sizes)
    except (AuditError, ValueError, TypeError, KeyError) as error:
        print(json.dumps(dict(status="FAIL", error=str(error))))
        return 1
    result["audit_wall_seconds"] = time.monotonic() - begin
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
