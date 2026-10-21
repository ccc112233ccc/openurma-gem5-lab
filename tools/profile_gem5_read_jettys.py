#!/usr/bin/env python3
"""Profile warmed, single-port 128-B READ bandwidth while varying Jettys."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import re
import shlex
import subprocess
import time

from run_gem5_rma_jetty_compare import command, parse_result
from run_modular_rma_regression import collect_fence_deltas, fence_logs, validate_fences


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lab", type=Path, required=True)
    parser.add_argument("--run-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jettys", type=int, nargs="+", default=[1, 2, 4, 1])
    parser.add_argument("--total-ops", type=int, default=32768)
    parser.add_argument("--aggregate-depth", type=int,
                        help="keep the total outstanding JFS/JFR depth fixed across Jettys")
    parser.add_argument("--client-preload", help="guest LD_PRELOAD phase profiler path")
    parser.add_argument("--client-binary", help="guest perftest diagnostic executable path")
    parser.add_argument("--client-env", help="guest environment variable assignment")
    parser.add_argument("--client-stats-helper",
                        help="guest binary accepting r/d to reset and dump gem5 stats around the client run")
    parser.add_argument("--client-library-path", help="override client LD_LIBRARY_PATH")
    parser.add_argument("--pin-cpu", type=int,
                        help="run both perftest peers on this guest CPU with taskset")
    parser.add_argument("--client-provider-profile", action="store_true",
                        help="require one UDMA_INNER provider timing row")
    parser.add_argument("--no-peak", action="store_true",
                        help="pass -N to both peers to disable per-operation peak timestamps")
    parser.add_argument("--timeout", type=float, default=900)
    args = parser.parse_args()
    if args.pin_cpu is not None and args.pin_cpu < 0:
        parser.error("--pin-cpu must be nonnegative")

    depth, jfc_depth, post_list, size = 8192, 524288, 256, 128
    if any(j < 1 or args.total_ops % j or
           (args.aggregate_depth and args.aggregate_depth % j) or
           args.total_ops // j < (args.aggregate_depth // j if args.aggregate_depth else depth)
           for j in args.jettys):
        parser.error("invalid Jetty count, iterations, or aggregate depth")
    if args.aggregate_depth and (args.aggregate_depth < max(args.jettys) * post_list or
                                 args.aggregate_depth > depth):
        parser.error("aggregate depth must cover each posting list and be <= 8192")
    manifest = (args.run_root / "run-manifest.txt").read_text()
    for required in ("profile=modern\n", "sync_request=roi\n",
                     "ub_port_count=1\n", "peer_link_rate_gbps=400\n"):
        if required not in manifest:
            parser.error(f"run manifest lacks {required.strip()}")

    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "run-manifest.txt").write_text(manifest)
    stats = [args.run_root / f"node{node}/stats.txt" for node in range(2)]
    paths = fence_logs(args.run_root)
    fields = ["sequence", "jettys", "iterations_per_jetty", "total_operations",
              "effective_jfs_depth", "peak_mib_s", "average_mib_s",
              "message_rate_mpps", "wall_seconds", "transcript",
              "phase_frequency_hz", "phase_span_ticks", "phase_post_jetty_calls",
              "phase_post_jetty_ticks", "phase_post_jfs_calls",
              "phase_post_jfs_ticks", "phase_poll_calls", "phase_poll_ticks",
              "phase_poll_empty_calls", "phase_poll_empty_ticks",
              "phase_poll_positive_calls", "phase_poll_positive_ticks",
              "phase_cqes", "phase_other_ticks",
              "phase_gap_post_post_ticks", "phase_gap_post_poll_ticks",
              "phase_gap_poll_post_ticks", "phase_gap_poll_poll_ticks",
              "phase_gap_post_post_calls", "phase_gap_post_poll_calls",
              "phase_gap_poll_post_calls", "phase_gap_poll_poll_calls",
              "phase_gap_post_post_max", "phase_gap_post_poll_max",
              "phase_gap_poll_post_max", "phase_gap_poll_poll_max",
              "phase_post_max", "phase_poll_max"]
    fields += ["loop_frequency_hz", "loop_outer_loops", "loop_jetty_visits",
               "loop_post_calls", "loop_poll_calls", "loop_cqes", "loop_empty_polls",
               "loop_scan_ticks", "loop_post_ticks", "loop_poll_ticks",
               "loop_account_ticks"]
    fields += ["inner_frequency_hz", "inner_post_calls", "inner_post_wrs",
               "inner_post_lock_calls", "inner_post_total_ticks", "inner_post_encode_ticks",
               "inner_post_barrier_ticks", "inner_post_db_ticks", "inner_poll_calls",
               "inner_poll_cqes", "inner_poll_empty_calls", "inner_poll_lock_calls",
               "inner_poll_total_ticks", "inner_poll_scan_ticks", "inner_poll_db_ticks"]
    fields += ["inner_cqe_samples", "inner_cqe_lookup_ticks",
               "inner_cqe_barrier_ticks", "inner_cqe_parse_ticks",
               "inner_cqe_timer_pair_ticks", "inner_cqe_relookup_ticks"]
    fields += ["inner_wqe_samples", "inner_wqe_check_ticks",
               "inner_wqe_set_ticks", "inner_wqe_queue_ticks",
               "inner_wqe_timer_pair_ticks"]
    fields += ["inner_sqe_samples", "inner_sqe_check_ticks",
               "inner_sqe_prepare_ticks", "inner_sqe_parse_ticks"]
    with (args.output / "results.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for index, jettys in enumerate(args.jettys):
            iterations = args.total_ops // jettys
            case_depth = args.aggregate_depth // jettys if args.aggregate_depth else depth
            port = 21700 + index
            server_command = command("read", size, jettys, iterations, port,
                                     None, case_depth, jfc_depth, post_list)
            client_command = command("read", size, jettys, iterations, port,
                                     "10.0.0.1", case_depth, jfc_depth, post_list)
            if args.no_peak:
                server_command += " -N"
                client_command += " -N"
            if args.client_preload:
                client_command = ("LD_PRELOAD=" + shlex.quote(args.client_preload)
                                  + " " + client_command)
            if args.client_binary:
                client_command = client_command.replace("urma_perftest", shlex.quote(args.client_binary), 1)
            if args.client_library_path:
                client_command = client_command.replace(
                    "LD_LIBRARY_PATH=/lib:/usr/lib",
                    "LD_LIBRARY_PATH=" + shlex.quote(args.client_library_path), 1)
            if args.pin_cpu is not None:
                pinned = f"taskset {1 << args.pin_cpu:x} "
                server_command = server_command.replace("urma_perftest", pinned + "urma_perftest", 1)
                client_command = client_command.replace(
                    shlex.quote(args.client_binary) if args.client_binary else "urma_perftest",
                    pinned + (shlex.quote(args.client_binary) if args.client_binary else "urma_perftest"), 1)
            if args.client_env:
                client_command = args.client_env + " " + client_command
            if args.client_stats_helper:
                helper = shlex.quote(args.client_stats_helper)
                client_command = (f"{helper} r && ( {client_command} ); "
                                  f"__ubsim_read_rc=$?; {helper} d; "
                                  "test $__ubsim_read_rc -eq 0")
            commands = (server_command, client_command)
            offsets = {name: path.stat().st_size if path.is_file() else None
                       for name, path in paths.items()}
            stat_offsets = [path.stat().st_size if path.is_file() else 0
                            for path in stats]
            started = time.perf_counter()
            result = subprocess.run(
                ["python3", str(args.lab / "tools/dual_serial_command.py"),
                 "--ports", "3460", "3470", "--commands", *commands,
                 "--timeout", str(args.timeout), "--prompt-kick-after", "1",
                 "--stagger", "1", "--full-output"],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
            elapsed = time.perf_counter() - started
            prefix = f"{index:02d}-read-s{size}-j{jettys}"
            transcript = f"{prefix}.uart.txt"
            (args.output / transcript).write_text(result.stdout)
            logs, errors = collect_fence_deltas(paths, offsets, args.output, prefix)
            errors.extend(validate_fences(logs))
            for node, (path, offset) in enumerate(zip(stats, stat_offsets)):
                with path.open("rb") as source:
                    source.seek(offset)
                    (args.output / f"{prefix}.node{node}.stats.txt").write_bytes(source.read())
            try:
                peak, average, mpps = parse_result(result.stdout, size,
                                                   jettys, args.total_ops)
            except ValueError as error:
                errors.append(str(error))
            phase = {}
            if args.client_preload:
                matches = re.findall(r"^\[URMA_PHASE\] (.*)$", result.stdout,
                                     re.MULTILINE)
                if len(matches) != 1:
                    errors.append(f"expected one URMA_PHASE row, got {len(matches)}")
                else:
                    phase = {"phase_" + key: value for key, value in
                             re.findall(r"(\w+)=(\d+)", matches[0])}
                    if int(phase.get("phase_cqes", "-1")) != args.total_ops:
                        errors.append(f"phase CQE count mismatch: {phase.get('phase_cqes')}")
            if args.client_binary:
                matches = re.findall(r"^\[URMA_LOOP\] (.*)$", result.stdout,
                                     re.MULTILINE)
                if len(matches) != 1:
                    errors.append(f"expected one URMA_LOOP row, got {len(matches)}")
                else:
                    phase.update({"loop_" + key: value for key, value in
                                  re.findall(r"(\w+)=(\d+)", matches[0])})
                    if int(phase.get("loop_cqes", "-1")) != args.total_ops:
                        errors.append(f"loop CQE count mismatch: {phase.get('loop_cqes')}")
            if args.client_provider_profile:
                matches = re.findall(r"^\[UDMA_INNER\] (.*)$", result.stdout,
                                     re.MULTILINE)
                if len(matches) != 1:
                    errors.append(f"expected one UDMA_INNER row, got {len(matches)}")
                else:
                    phase.update({"inner_" + key: value for key, value in
                                  re.findall(r"(\w+)=(\d+)", matches[0])})
                    if int(phase.get("inner_poll_cqes", "-1")) != args.total_ops:
                        errors.append(f"provider CQE count mismatch: {phase.get('inner_poll_cqes')}")
            if result.returncode or errors:
                print(f"J={jettys} failed: rc={result.returncode}, errors={errors}", flush=True)
                return 1
            writer.writerow({"sequence": index + 1, "jettys": jettys,
                             "iterations_per_jetty": iterations,
                             "total_operations": args.total_ops,
                             "effective_jfs_depth": case_depth,
                             "peak_mib_s": peak, "average_mib_s": average,
                             "message_rate_mpps": mpps,
                             "wall_seconds": f"{elapsed:.3f}",
                             "transcript": transcript, **phase})
            stream.flush()
            print(f"J={jettys}: {average:.2f} MiB/s, {mpps:.3f} Mops/s, "
                  f"wall {elapsed:.1f}s", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
