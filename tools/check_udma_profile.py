#!/usr/bin/env python3
"""Check final UDMA observations from a bounded, stopped regression run."""
import argparse
import json
from pathlib import Path
import re


def check_profile(text, expected_wqes, min_rma_peak=2):
    lines = [line for line in text.splitlines() if line.startswith("[UDMA_PROFILE]")]
    if len(lines) != 1:
        return {}, ["expected exactly one final UDMA profile"]
    fields = dict(re.findall(r"(\w+)=(0x[0-9a-fA-F]+|\d+)", lines[0]))
    metrics = {key: int(value, 0) for key, value in fields.items()}
    errors = []
    for key in ("sq_wqes", "sq_completions"):
        if metrics.get(key) != expected_wqes:
            errors.append(f"{key}: expected {expected_wqes}, got {metrics.get(key)}")
    for key in ("sq_depth_rejects", "sq_decode_rejects", "ubase_errors"):
        if metrics.get(key) != 0:
            errors.append(f"{key}: expected zero, got {metrics.get(key)}")
    for key, minimum in (("rma_pending_peak", min_rma_peak),
                         ("sq_wqebbs_retired", expected_wqes),
                         ("sq_consumer_wraps", 1)):
        if metrics.get(key, -1) < minimum:
            errors.append(f"{key}: expected >= {minimum}, got {metrics.get(key)}")
    return metrics, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", type=Path, nargs="+")
    parser.add_argument("--expected-wqes", type=int, required=True)
    parser.add_argument("--min-rma-peak", type=int, default=2)
    args = parser.parse_args()
    reports = []
    for path in args.logs:
        metrics, errors = check_profile(path.read_text(), args.expected_wqes,
                                        args.min_rma_peak)
        reports.append({"path": str(path), "metrics": metrics, "errors": errors})
    print(json.dumps(reports, indent=2))
    return int(any(report["errors"] for report in reports))


if __name__ == "__main__":
    raise SystemExit(main())
