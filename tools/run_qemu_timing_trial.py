#!/usr/bin/env python3
"""Bounded timing-mode QEMU bandwidth trial; preserves commands and evidence."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import time
from run_modular_rma_regression import validate_depth_output


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("output", type=Path)
    p.add_argument("--timeout", type=float, default=180)
    p.add_argument("--sizes", type=int, nargs="+", default=[4096, 65536, 1048576])
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
