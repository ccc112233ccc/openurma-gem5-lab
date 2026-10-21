#!/usr/bin/env python3
"""Build the lab's explicit optimized UDMA provider from pinned UMDK sources."""

from pathlib import Path
import hashlib
import json
import shutil
import subprocess

from build_udma_hotpath_probes import ROOT, SOURCE, build

PINNED_UMDK_SHA = "8f272493e4138cd52cfb3ce11064a07c8d1be49f"


def main() -> None:
    umdk = SOURCE.parents[3]
    revision = subprocess.check_output(
        ["git", "-C", str(umdk), "rev-parse", "HEAD"], text=True).strip()
    if revision != PINNED_UMDK_SHA:
        raise RuntimeError(f"unexpected UMDK revision: {revision}")
    subprocess.run(["git", "-C", str(umdk), "diff", "--quiet", "--",
                    "src/urma/hw/udma"], check=True)
    source = build("optimized", prefetch_distance=4,
                   sqe_prefetch_distance=8, optimize_o3=True)
    output = ROOT / "artifacts/udma-optimized-provider"
    output.mkdir(parents=True, exist_ok=True)
    target = output / "liburma-udma.so"
    shutil.copy2(source, target)
    sha = hashlib.sha256(target.read_bytes()).hexdigest()
    (output / "manifest.json").write_text(json.dumps({
        "provider": "udma-optimized",
        "base": "pinned UMDK with CQE read-ahead, SQE write-ahead and -O3 on two objects",
        "cqe_prefetch_distance": 4,
        "sqe_prefetch_distance": 8,
        "sha256": sha,
        "source": str(source.relative_to(ROOT)),
    }, indent=2) + "\n")
    print(f"optimized UDMA provider: {target}\nsha256: {sha}")


if __name__ == "__main__":
    main()
