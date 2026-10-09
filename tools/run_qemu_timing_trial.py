#!/usr/bin/env python3
"""Bounded timing-mode QEMU bandwidth trial; preserves commands and evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time
import re
import os
import signal
from run_modular_rma_regression import validate_depth_output, validate_bw_scan


def write_bw_command(size, port, iterations=64, depth=64, post_list=16, cq_mod=16,
                     inline_size=64):
    return bw_command("write_bw", size, port, iterations, depth, post_list, cq_mod, inline_size)


def bw_command(verb, size, port, iterations=64, depth=64, post_list=16, cq_mod=16,
               inline_size=64, report_diagnostic=False, unidirectional_read=False):
    if unidirectional_read and (verb != "read_bw" or report_diagnostic):
        raise ValueError("single-direction mode supports only READ without report diagnostics")
    if verb not in ("write_bw", "read_bw", "send_bw"):
        raise ValueError("unsupported bandwidth verb")
    if min(size, port, iterations, depth, post_list, cq_mod) <= 0:
        raise ValueError("bandwidth settings must be positive")
    if depth > iterations:
        raise ValueError("depth must not exceed iterations (perftest would silently lower it)")
    if verb == "send_bw" and size > 4096:
        raise ValueError("SEND payload must not exceed 4096 bytes")
    if post_list > depth or cq_mod > depth:
        raise ValueError("WRITE post list and CQ moderation must not exceed SQ depth")
    if iterations % post_list or iterations % cq_mod:
        raise ValueError("WRITE iterations must be divisible by post list and CQ moderation")
    if post_list > 1 and post_list % cq_mod:
        raise ValueError("post list must be a multiple of CQ moderation")
    if inline_size < 0:
        raise ValueError("WRITE inline size must not be negative")
    if verb == "read_bw":
        inline_size = 0  # Official perftest READ unconditionally disables inline.
    receive_depth = f" -R {depth}" if verb != "write_bw" else ""
    diagnostic_env = "UBSIM_BW_REPORT_DIAGNOSTIC=1 " if report_diagnostic else ""
    direction = "" if unidirectional_read else "-B "
    return (f"{diagnostic_env}LD_LIBRARY_PATH=/lib:/usr/lib urma_perftest {verb} "
            f"-d udma0 --eid_idx 0 --ctp {direction}-s {size} -P {port} "
            f"-J 1 -I {inline_size} -T {depth}{receive_depth} -n {iterations} -l {post_list} -Q {cq_mod} -p 0")


def validate_unidirectional_read_output(output, depth, size, iterations):
    sections = re.split(r"(?m)^--- UART \d+ ---\s*$", output)[1:]
    if len(sections) != 2:
        return ["expected two UART transcripts"]
    errors = []
    number = r"\d+(?:\.\d+)?(?:[eE][+-]?\d+)?"
    for node, section in enumerate(sections):
        depths = re.findall(r"JFS depth\s*:\s*(\d+)", section)
        if depths != ([] if node == 0 else [str(depth)]):
            errors.append(f"node{node}: unexpected single-direction READ depth header {depths}")
        row = rf"(?m)^\s*{size}\s+{iterations}\s+{number}\s+{number}\s+{number}\s*$"
        if len(re.findall(row, section)) != 1:
            errors.append(f"node{node}: missing or duplicate complete bandwidth result")
    return errors


def observe_drain(root, timeout):
    """Sample live process-owned work; not a coordinated checkpoint barrier."""
    begin = time.monotonic()
    specs = [(f"udma-node{n}/udma", "UDMA_PROGRESS") for n in (0, 1)]
    specs.append(("ub-fabric/ns3", "NS3_UB_NET_PROGRESS"))
    samples = []
    consecutive = 0
    while time.monotonic() - begin < timeout:
        offsets = {}
        for component, marker in specs:
            path = root / f"{component}.log"
            offsets[component] = path.stat().st_size
            os.kill(int((root / f"{component}.pid").read_text()), signal.SIGUSR1)
        replies = {}
        deadline = min(begin + timeout, time.monotonic() + 1)
        while time.monotonic() < deadline:
            for component, marker in specs:
                with (root / f"{component}.log").open("rb") as stream:
                    stream.seek(offsets[component])
                    lines = stream.read().decode(errors="replace").splitlines(keepends=True)
                for line in lines:
                    if line.endswith("\n") and line.startswith(f"[{marker}]"):
                        replies[component] = {k: int(v) for k, v in
                            re.findall(r"(\w+)=(\d+)", line)}
            if len(replies) == len(specs):
                break
            time.sleep(0.02)
        drained = all(replies.get(f"udma-node{n}/udma", {}).get("quiescent") == 1
                      for n in (0, 1))
        fabric = replies.get("ub-fabric/ns3", {})
        drained = drained and all(fabric.get(k) == 0 for k in
            ("native_tasks", "pending_target_dma", "scheduled_packets"))
        drained = drained and fabric.get("outputs_empty") == 1
        samples.append(dict(wall_seconds=time.monotonic() - begin,
                            processes=replies, observed_work_drained=drained))
        consecutive = consecutive + 1 if drained else 0
        if consecutive >= 2:
            break
        time.sleep(min(0.25, max(0, begin + timeout - time.monotonic())))
    return dict(label="post-workload-drain", wall_seconds=time.monotonic() - begin,
                observed_work_drained=consecutive >= 2, samples=samples,
                scope="Live work snapshots, not global event-queue or checkpoint quiescence")


def validate_write_capabilities(text, depth, inline_size):
    return validate_bw_capabilities(text, "write_bw", depth, inline_size)


def validate_bw_capabilities(text, verb, depth, inline_size, max_size=None):
    sections = re.split(r"(?m)^--- UART \d+ ---\s*$", text)[1:]
    if len(sections) != 2:
        raise ValueError("need capability responses from both guests")
    limits = []
    for node, section in enumerate(sections):
        values = {}
        requested_limits = [("max_jfs_depth", depth),
                            ("max_jfs_inline_size", 0 if verb == "read_bw" else inline_size)]
        if verb != "write_bw":
            requested_limits.append(("max_jfr_depth", depth))
        if max_size is not None:
            requested_limits.append(("max_msg_size", max_size))
        for field, requested in requested_limits:
            found = re.findall(r"(?m)^" + field + r"\s*:\s*(\d+)\s*$", section)
            if len(found) != 1:
                raise ValueError(f"node{node}: missing unique {field}")
            values[field] = int(found[0])
            if requested > values[field]:
                raise ValueError(f"node{node}: requested {requested} exceeds {field}={values[field]}")
        limits.append(values)
    return limits


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("output", type=Path)
    p.add_argument("--timeout", type=float, default=180)
    p.add_argument("--sizes", type=int, nargs="+", default=[4096, 65536])
    p.add_argument("--drain-timeout", type=float, default=0,
                   help="optional bounded live drain observation before stopping processes")
    p.add_argument("--bw-verb", choices=("write_bw", "read_bw", "send_bw"), default="write_bw")
    p.add_argument("--unidirectional-read", action="store_true",
                   help="omit -B for explicit-size READ; server prints the client's exchanged result")
    p.add_argument("--report-diagnostic", action="store_true",
                   help="request existing-sample diagnostics from an explicitly selected instrumented initrd")
    p.add_argument("--iterations", "--write-iterations", dest="write_iterations", type=int, default=64)
    p.add_argument("--depth", "--write-depth", dest="write_depth", type=int, default=64)
    p.add_argument("--post-list", "--write-post-list", dest="write_post_list", type=int, default=16)
    p.add_argument("--cq-mod", "--write-cq-mod", dest="write_cq_mod", type=int, default=16)
    p.add_argument("--inline", "--write-inline", dest="write_inline", type=int, default=64,
                   help="maximum inline bytes (-I), independent of SQ depth (-T)")
    p.add_argument("--user-read-scan", action="store_true",
                   help="run requested a23/n16384 READ scan, capped to reported queue limits")
    p.add_argument("--read-size", type=int,
                   help="with --user-read-scan, isolate one size instead of a23")
    p.add_argument("--read-sizes", type=int, nargs="+",
                   help="test separate READ sizes; continue after clean guest errors")
    args = p.parse_args(argv)
    if args.unidirectional_read and (args.bw_verb != "read_bw" or args.user_read_scan or args.report_diagnostic):
        p.error("--unidirectional-read requires explicit-size read_bw without diagnostics")
    if args.report_diagnostic and args.user_read_scan:
        p.error("report diagnostics require explicit-size bandwidth trials")
    for sizes in (args.sizes, args.read_sizes or []):
        if len(sizes) != len(set(sizes)):
            p.error("duplicate sizes would overwrite experiment evidence")
    if not args.user_read_scan:
        try:
            for size in args.sizes:
                bw_command(args.bw_verb, size, 21410, args.write_iterations, args.write_depth,
                                 args.write_post_list, args.write_cq_mod, args.write_inline,
                                 unidirectional_read=args.unidirectional_read)
        except ValueError as error:
            p.error(str(error))
    return args


def main():
    args = parse_args()
    lab = Path(__file__).resolve().parents[1]
    root = lab / "run-qemu-dual"
    args.output.mkdir(parents=True, exist_ok=False)
    records = []
    launched = False
    start = time.monotonic()
    def run(label, command, timeout):
        begin = time.monotonic()
        try:
            result = subprocess.run(command, cwd=lab, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=timeout)
            text, rc = result.stdout, result.returncode
        except subprocess.TimeoutExpired as exc:
            text = exc.stdout or b""
            if isinstance(text, bytes): text = text.decode(errors="replace")
            rc = 124
        (args.output / f"{label}.txt").write_text(text)
        records.append(dict(label=label, command=command, returncode=rc,
                            wall_seconds=time.monotonic()-begin))
        print(f"{label}: rc={rc} wall={records[-1]['wall_seconds']:.3f}s", flush=True)
        return rc
    try:
        initrd = Path(os.environ.get("UBSIM_INITRD", str(lab / "out/official-udma.cpio.gz")))
        if args.report_diagnostic and "UBSIM_INITRD" not in os.environ:
            raise ValueError("diagnostics require explicit UBSIM_INITRD; the regular image is not instrumented")
        records.append(dict(label="input-image", path=str(initrd.resolve()),
                            sha256=hashlib.sha256(initrd.read_bytes()).hexdigest(),
                            report_diagnostic=args.report_diagnostic))
        if run("launch", ["./lab", "start-qemu-dual", "--timing"], 30):
            return 1
        launched = True
        shutil.copy(root / "run-manifest.txt", args.output / "run-manifest.txt")
        manifest = dict(line.split("=", 1) for line in
                        (root / "run-manifest.txt").read_text().splitlines() if "=" in line)
        artifacts = {}
        for name in ("qemu_binary", "udma_device_binary", "ns3_binary", "kernel_image", "initrd_image"):
            path = Path(manifest[name])
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            artifacts[name] = dict(path=str(path.resolve()), sha256=digest.hexdigest())
        records.append(dict(label="input-artifacts", artifacts=artifacts,
                            scope="Launch executable/images only; dynamic dependencies not fingerprinted"))
        serial = ["python3", "tools/dual_serial_command.py", "--ports", "3560", "3570",
                  "--full-output", "--timeout", str(args.timeout), "--prompt-kick-after", "1"]
        # Wait for kernel/init shell-ready banners before injecting a newline.
        begin = time.monotonic()
        while not all("Official UDMA full-system guest" in
                      (root/f"node{n}/system.terminal").read_text(errors="replace")
                      for n in (0, 1)):
            if time.monotonic()-begin > args.timeout:
                raise TimeoutError("shell-ready deadline")
            time.sleep(0.25)
        records.append(dict(label="shell-ready", wall_seconds=time.monotonic()-start,
                            returncode=0))
        print(f"shell-ready: {time.monotonic()-start:.3f}s", flush=True)
        if run("network", serial+["--command", "ubsim-net-up"], args.timeout+10): return 1
        if args.user_read_scan:
            if run("capabilities", serial+["--command", "urma_admin show -d udma0 -w"],
                   args.timeout+10): return 1
            caps = (args.output/"capabilities.txt").read_text()
            selected = {}
            for option, name, requested in (("R", "max_jfr_depth", 8192),
                                             ("T", "max_jfs_depth", 8192),
                                             ("C", "max_jfc_depth", 524288)):
                values = [int(v) for v in re.findall(name+r"\s*:\s*(\d+)", caps)]
                if len(values) != 2 or min(values) <= 0:
                    raise ValueError(f"missing both guests' capability: {name}")
                selected[option] = min(requested, *values)
            print(f"requested READ scan effective queue settings: {selected}", flush=True)
            if selected["T"] < 256:
                raise ValueError("reported JFS capacity cannot support requested batch256")
            failed = False
            for index, read_size in enumerate(args.read_sizes or [args.read_size]):
                size_option = f"-s {read_size}" if read_size else "-a23"
                label = f"read-{read_size}" if args.read_sizes else "requested-read-scan"
                command = ("LD_LIBRARY_PATH=/lib:/usr/lib urma_perftest read_bw "
                           f"-d udma0 --eid_idx 0 --ctp {size_option} -P {21115+index} -J 1 "
                           f"-R {selected['R']} -T {selected['T']} -C {selected['C']} "
                           "-Q 1 -l 256 -n 16384 -p 0")
                rc = run(label, serial+["--stagger", "1", "--commands",
                         command, command+" -S 10.0.0.1"], args.timeout+10)
                transcript = (args.output/f"{label}.txt").read_text()
                # Single-direction READ server omits its JFS-depth header.
                if read_size:
                    sections = re.split(r"(?m)^--- UART \d+ ---\s*$", transcript)[1:]
                    row = rf"(?m)^\s*{read_size}\s+16384\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*$"
                    rows = [re.findall(row, section) for section in sections]
                    errors = [] if len(rows) == 2 and all(len(r) == 1 for r in rows) else ["missing single-size READ results"]
                    if not errors and rc == 0:
                        records[-1]["average_mib_s"] = [float(r[0][1]) for r in rows]
                else:
                    errors = validate_bw_scan(transcript, 23, 16384)
                records[-1]["validation_errors"] = errors
                records[-1]["size_bytes"] = read_size
                failed |= bool(rc or errors)
                if rc or errors:
                    # Reuse only guests that returned a clean command status;
                    # a timed-out process or dead adapter makes later data invalid.
                    statuses = re.findall(r"(?m)^__UBSIM_RC_\d+_\d+__=\d+\s*$", transcript)
                    if rc == 124 or len(statuses) != 2:
                        break
                    for component in ("node0/qemu", "node1/qemu", "udma-node0/udma",
                                      "udma-node1/udma", "ub-fabric/ns3"):
                        os.kill(int((root/f"{component}.pid").read_text().strip()), 0)
            return int(failed)
        if run("capabilities", serial+["--command", "urma_admin show -d udma0 -w"],
               args.timeout+10): return 1
        try:
            records[-1]["bw_capabilities"] = validate_bw_capabilities(
                (args.output / "capabilities.txt").read_text(), args.bw_verb,
                args.write_depth, args.write_inline, max(args.sizes))
            if args.bw_verb == "write_bw":
                records[-1]["write_capabilities"] = records[-1]["bw_capabilities"]
            records[-1]["effective_inline"] = 0 if args.bw_verb == "read_bw" else args.write_inline
        except ValueError as error:
            records[-1]["validation_errors"] = [str(error)]
            records[-1]["returncode"] = 1
            print(f"{args.bw_verb} capability preflight failed: {error}", flush=True)
            return 1
        for index, size in enumerate(args.sizes):
            command = bw_command(args.bw_verb, size, 21410 + index, args.write_iterations,
                                       args.write_depth, args.write_post_list, args.write_cq_mod,
                                       args.write_inline, args.report_diagnostic,
                                       args.unidirectional_read)
            label = f"{args.bw_verb.removesuffix('_bw')}-{size}"
            if run(label, serial+["--stagger", "1", "--commands",
                     command, command+" -S 10.0.0.1"], args.timeout+10): return 1
            validate_output = (validate_unidirectional_read_output if args.unidirectional_read
                               else validate_depth_output)
            errors = validate_output(
                (args.output/f"{label}.txt").read_text(), args.write_depth,
                size, args.write_iterations)
            if args.report_diagnostic:
                diagnostic_lines = re.findall(r"(?m)^\[UBSIM_BW_REPORT_DIAGNOSTIC\].*$",
                    (args.output/f"{label}.txt").read_text())
                if len(diagnostic_lines) != 2:
                    errors.append("missing unique report diagnostic from both endpoints")
            records[-1]["validation_errors"] = errors
            if errors:
                records[-1]["returncode"] = 1
                return 1
        return 0
    finally:
        if launched:
            if args.drain_timeout > 0:
                try:
                    drain = observe_drain(root, args.drain_timeout)
                    records.append(drain)
                    print(f"drain: observed={drain['observed_work_drained']} "
                          f"wall={drain['wall_seconds']:.3f}s", flush=True)
                except (OSError, ValueError) as error:
                    records.append(dict(label="post-workload-drain", error=str(error),
                                        observed_work_drained=False))
            run("stop", ["./lab", "stop-qemu"], 15)
            time.sleep(1)
        for node in (0, 1):
            for folder, name in ((f"node{node}", "system.terminal"),
                                 (f"node{node}", "qemu.log"),
                                 (f"udma-node{node}", "udma.log")):
                source = root / folder / name
                if source.exists(): shutil.copy(source, args.output/f"{folder}-{name}")
        if (root/"ub-fabric/ns3.log").exists():
            shutil.copy(root/"ub-fabric/ns3.log", args.output/"ns3.log")
        (args.output/"report.json").write_text(json.dumps(dict(
            wall_seconds=time.monotonic()-start, records=records), indent=2)+"\n")


if __name__ == "__main__":
    raise SystemExit(main())
