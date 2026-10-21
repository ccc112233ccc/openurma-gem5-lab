#!/usr/bin/env python3
"""Transfer a small bootstrap ELF over a checkpointed gem5 guest UART."""

import argparse
import binascii
import gzip
from pathlib import Path
import shlex
import threading
import time

from dual_serial_command import run_one


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--dest", required=True)
    args = parser.parse_args()
    compressed = gzip.compress(args.source.read_bytes(), mtime=0)
    lines = [binascii.b2a_uu(compressed[i:i + 45]).decode("ascii")
             for i in range(0, len(compressed), 45)]
    encoded = "begin 644 payload.gz\n" + "".join(lines) + "`\nend\n"
    destination = shlex.quote(args.dest)
    def send(command: str) -> str:
        for attempt in range(5):
            if attempt:
                time.sleep(1)
            results: dict[int, str] = {}
            run_one(args.port, command, 45, 0, 1, threading.Barrier(1), results)
            transcript = results.get(args.port, "ERROR: UART did not finish")
            if "terminal already attached" in transcript:
                continue
            if transcript.startswith("ERROR:"):
                raise RuntimeError(transcript[-800:])
            time.sleep(0.5)
            return transcript
        raise RuntimeError("ERROR: gem5 UART remained attached after retries")

    try:
        send(": > /tmp/ubsim-bootstrap.uu")
        # The gem5 UART can drop a multi-kilobyte input burst.  Complete each
        # short append and observe its shell prompt before sending the next.
        for offset in range(0, len(encoded), 150):
            chunk = encoded[offset:offset + 150]
            send("printf '%s' " + shlex.quote(chunk)
                 + " >> /tmp/ubsim-bootstrap.uu")
        transcript = send("uudecode -o /tmp/ubsim-bootstrap.gz "
                          "/tmp/ubsim-bootstrap.uu && "
                          f"gzip -dc /tmp/ubsim-bootstrap.gz > {destination} && "
                          f"chmod 755 {destination} && wc -c {destination}")
    except RuntimeError as exc:
        print(str(exc))
        return 1
    print(transcript[-800:])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
