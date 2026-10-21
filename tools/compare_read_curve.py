#!/usr/bin/env python3
"""Compare synchronized READ measurements with the supplied physical curve."""

import argparse
import csv
from pathlib import Path


def load_rows(path: Path) -> dict[int, dict[str, float]]:
    rows = {}
    with path.open(newline="") as stream:
        for row in csv.DictReader(stream):
            size = int(row["size_bytes"])
            if size in rows:
                raise ValueError(f"duplicate size {size} in {path}")
            rows[size] = {
                "average_mib_s": float(row["average_mib_s"]),
                "message_rate_mpps": float(row["message_rate_mpps"]),
            }
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("simulated", type=Path,
                        help="CSV with size_bytes,average_mib_s,message_rate_mpps")
    parser.add_argument("--physical", type=Path,
                        default=Path(__file__).resolve().parents[1] /
                        "results/physical-read-bw-20261009.csv")
    args = parser.parse_args()
    physical = load_rows(args.physical)
    simulated = load_rows(args.simulated)
    if not simulated:
        raise ValueError("simulated CSV has no rows")
    if unknown := simulated.keys() - physical.keys():
        raise ValueError(f"sizes absent from physical data: {sorted(unknown)}")

    print("size_bytes,physical_mib_s,simulated_mib_s,bandwidth_error_pct,"
          "physical_mpps,simulated_mpps,rate_error_pct,within_20_pct")
    passed = 0
    for size in sorted(simulated):
        actual, predicted = physical[size], simulated[size]
        bw_error = abs(predicted["average_mib_s"] / actual["average_mib_s"] - 1) * 100
        rate_error = abs(predicted["message_rate_mpps"] /
                         actual["message_rate_mpps"] - 1) * 100
        within = bw_error <= 20 and rate_error <= 20
        passed += within
        print(f"{size},{actual['average_mib_s']:.6f},"
              f"{predicted['average_mib_s']:.6f},{bw_error:.3f},"
              f"{actual['message_rate_mpps']:.6f},"
              f"{predicted['message_rate_mpps']:.6f},{rate_error:.3f},"
              f"{int(within)}")
    print(f"# passing_sizes={passed}/{len(simulated)} "
          f"physical_coverage={len(simulated)}/{len(physical)}")


if __name__ == "__main__":
    main()
