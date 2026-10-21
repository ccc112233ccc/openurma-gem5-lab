#!/usr/bin/env python3
"""Byte-level official URMA API checks on already-ready no-sync QEMU guests."""
import argparse
import json
import hashlib
import os
from pathlib import Path
import re
import subprocess
import time


def validate_output(text, op, size, rounds, batch, depth, concurrent=False, shared_jettys=1, negative=False):
    sections = re.split(r"(?m)^--- UART \d+ ---\s*$", text)[1:]
    if len(sections) != 2:
        return ["expected two UART transcripts"]
    errors = []
    for section, role in zip(sections, ("server", "client")):
        expected = (f"PASS integrity op={op} bytes={size} rounds={rounds} batch={batch} "
                    f"depth={depth} shared_jettys={shared_jettys} schedule=round-robin-pairs "
                    f"payloads={rounds * batch} role={role}")
        if negative:
            expected = f"PASS negative-send-length bytes=4097 expected_status=2 receiver_unchanged=1 role={role}"
        markers = [line.strip() for line in section.splitlines() if line.startswith("PASS ")]
        if markers != [expected] * (2 if concurrent else 1):
            errors.append(f"{role}: incorrect or missing acceptance marker: {markers}")
    return errors


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("output", type=Path)
    p.add_argument("--suite", choices=("smoke", "boundaries", "wrap", "concurrent", "shared", "large", "send-overlimit"), default="smoke")
    p.add_argument("--timeout", type=float, default=90)
    args = p.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=False)
    run_root = Path(os.environ.get("UBSIM_QEMU_DUAL_OUT", root / "run-qemu-dual"))
    manifest = (run_root / "run-manifest.txt").read_text()
    if "synchronization=off" not in manifest.splitlines():
        raise RuntimeError("functional integrity runner requires no-sync")
    (args.output / "model-manifest.txt").write_text(manifest)
    binary = (root / "artifacts/urma-integrity").read_bytes()
    started = time.monotonic()
    for port in (2220, 2221):
        subprocess.run(["ssh", "-p", str(port), "-o", "BatchMode=yes",
                        "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                        "root@127.0.0.1", "dd of=/tmp/urma-integrity 2>/dev/null && chmod 755 /tmp/urma-integrity"],
                       input=binary, check=True)
    if args.suite == "boundaries":
        cases = [(op, size, 4, 1, 64) for op in ("send", "write", "read")
                 for size in (1, 2, 63, 64, 65, 127, 128, 129, 4095, 4096,
                              *(() if op == "send" else (4097, 65535, 65536)))]
    elif args.suite in ("wrap", "shared"):
        cases = [(op, 4096, 8, 512, 512) for op in ("send", "write", "read")]
    elif args.suite == "large":
        cases = [(op, size, 4, 1, 64) for op in ("write", "read") for size in (65537, 1048576)]
    elif args.suite == "send-overlimit":
        cases = [("send", 4097, 1, 1, 512)]
    else:
        cases = [(op, 4096, 4, 1, 64) for op in ("send", "write", "read")]
    records = []
    for index, (op, size, rounds, batch, depth) in enumerate(cases):
        port = 21500 + index * 2
        shared = 2 if args.suite == "shared" else 1
        base = f"UBSIM_INTEGRITY_SHARED_JETTYS={shared} LD_LIBRARY_PATH=/lib:/usr/lib /tmp/urma-integrity udma0 {op} {size} {rounds} {batch} {depth}"
        if args.suite == "send-overlimit":
            base = "UBSIM_INTEGRITY_EXPECT_SEND_LENGTH_ERROR=1 " + base
        commands = []
        for node in (0, 1):
            peer = " 10.0.0.1" if node else ""
            if args.suite == "concurrent":
                # Separate processes/contexts and different data seeds, checked
                # independently before emitting either captured transcript.
                command = (f"{base} {port}{peer} >/tmp/integrity-a.log 2>&1 & a=$!; "
                           f"{base} {port+1}{peer} >/tmp/integrity-b.log 2>&1 & b=$!; "
                           "wait $a; ra=$?; wait $b; rb=$?; cat /tmp/integrity-a.log /tmp/integrity-b.log; "
                           "test $ra -eq 0 && test $rb -eq 0")
            else:
                command = f"{base} {port}{peer}"
            commands.append(command)
        begin = time.monotonic()
        result = subprocess.run(["python3", str(root / "tools/dual_serial_command.py"),
                                 "--ports", "3560", "3570", "--commands", *commands,
                                 "--timeout", str(args.timeout), "--stagger", "1",
                                 "--prompt-kick-after", "1", "--full-output"],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        name = f"{index:02d}-{op}-{size}.uart.txt"
        (args.output / name).write_text(result.stdout)
        # Exact marker format is emitted only after data checks and teardown.
        errors = validate_output(result.stdout, op, size, rounds, batch, depth, args.suite == "concurrent", shared, args.suite == "send-overlimit")
        accepted = result.returncode == 0 and not errors
        records.append(dict(op=op, size=size, rounds=rounds, batch=batch, depth=depth,
                            commands=commands, returncode=result.returncode,
                            wall_seconds=time.monotonic()-begin, accepted=accepted, validation_errors=errors, raw_output=name))
        print(f"{op}/{size}: accepted={accepted} wall={records[-1]['wall_seconds']:.3f}s", flush=True)
        if not accepted:
            break
    report = dict(suite=args.suite, expected_cases=len(cases), cases=records,
                  binary_sha256=hashlib.sha256(binary).hexdigest(),
                  wall_seconds=time.monotonic()-started)
    (args.output / "report.json").write_text(json.dumps(report, indent=2)+"\n")
    return int(len(records) != len(cases) or not all(x["accepted"] for x in records))


if __name__ == "__main__":
    raise SystemExit(main())
