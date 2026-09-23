#!/usr/bin/env python3
"""Run the common lifetime adapter contract against native ns-3-UB."""

import pathlib
import sys

from test_ub_switch_sim import run_case


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} NS3_UB_ADAPTER_BINARY", file=sys.stderr)
        return 2
    binary = pathlib.Path(sys.argv[1]).resolve()
    run_case(binary, 2, 1, "--native-multi")
    run_case(binary, 4, 1, "--native-multi")
    run_case(binary, 2, 2, "--native-multi")
    print("native ns-3-UB lifetime adapter synchronization test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
