#!/usr/bin/env python3
"""Bounded timing-mode QEMU bandwidth trial; preserves commands and evidence."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import time
import re
import os
from run_modular_rma_regression import validate_depth_output, validate_bw_scan


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("output", type=Path)
    p.add_argument("--timeout", type=float, default=180)
    p.add_argument("--sizes", type=int, nargs="+", default=[4096, 65536, 1048576])
    p.add_argument("--user-read-scan", action="store_true",
                   help="run requested a23/n16384 READ scan, capped to reported queue limits")
    p.add_argument("--read-size", type=int,
                   help="with --user-read-scan, isolate one size instead of a23")
    p.add_argument("--read-sizes", type=int, nargs="+",
                   help="test separate READ sizes; continue after clean guest errors")
    args = p.parse_args()
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
        if run("launch", ["./lab", "start-qemu-dual", "--timing"], 30):
            return 1
        launched = True
        shutil.copy(root / "run-manifest.txt", args.output / "run-manifest.txt")
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
        for index, size in enumerate(args.sizes):
            command = ("LD_LIBRARY_PATH=/lib:/usr/lib urma_perftest write_bw "
                       f"-d udma0 --eid_idx 0 --ctp -B -s {size} -P {21410+index} "
                       "-J 1 -I 64 -T 64 -n 64 -l 16 -Q 16 -p 0")
            if run(f"write-{size}", serial+["--stagger", "1", "--commands",
                     command, command+" -S 10.0.0.1"], args.timeout+10): return 1
            errors = validate_depth_output(
                (args.output/f"write-{size}.txt").read_text(), 64, size, 64)
            records[-1]["validation_errors"] = errors
            if errors:
                records[-1]["returncode"] = 1
                return 1
        return 0
    finally:
        if launched:
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
