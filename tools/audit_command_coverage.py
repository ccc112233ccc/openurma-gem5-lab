#!/usr/bin/env python3
"""Inventory pinned driver enums against reviewed model behavior (not execution proof)."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
UDMA = "oe66/drivers/ub/urma/hw/udma/udma_cmd.h"
GROUPS = [
    ("ubase", "oe66/include/ub/ubase/ubase_comm_cmd.h", "ubase_opcode_type"),
    ("ubase", UDMA, "udma_cmd_opcode_type"),
    ("mailbox", UDMA, ""),
    ("mailbox", "oe66/include/ub/ubase/ubase_comm_mbx.h", "ubase_mbox_opcode"),
    ("ctrlq-tp", "oe66/drivers/ub/urma/hw/udma/udma_ctrlq_tp.h", "udma_ctrlq_cmd_code_type"),
    ("ctrlq-device", UDMA, "udma_ctrlq_dev_mgmt_opcode"),
]
# These classifications are deliberately conservative and reviewed by hand.
# A modeled command can still have partial semantics; see the coverage document.
MODELED = {
    "ubase": {0x1, 0x2, 0x30, 0x6200, 0x7001},
    "mailbox": {0x4, 0x6, 0x7, 0x24, 0x25, 0x27, 0x34, 0x37, 0x44, 0x47, 0x54, 0x57},
    "ctrlq-tp": {0x13, 0x21, 0x22, 0x23},
    "ctrlq-device": {0x1},
}


def entries():
    for namespace, path, enum_name in GROUPS:
        text = re.sub(r"/\*.*?\*/", "", (ROOT / path).read_text(), flags=re.S)
        match = re.search(r"\benum\s+" + re.escape(enum_name) + r"\s*\{([^}]+)\}", text, re.S)
        if not match:
            raise ValueError(f"missing enum {enum_name!r} in {path}")
        value = -1
        for item in match[1].split(","):
            item = item.strip()
            if not item:
                continue
            parts = item.split("=", 1)
            symbol = parts[0].strip()
            value = int(parts[1].strip(), 0) if len(parts) == 2 else value + 1
            if symbol.endswith("_MAX"):
                continue
            if namespace == "ubase" and value in {0x7000, 0xf00e}:
                status = "dispatch-only"
            elif value in MODELED[namespace]:
                status = "modeled-partial"
            elif namespace.startswith("ctrlq"):
                status = "rejected"
            else:
                status = "completion-only"
            yield {"namespace": namespace, "symbol": symbol, "opcode": hex(value),
                   "classification": status, "source": path}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="Print every symbol and classification")
    args = parser.parse_args()
    rows = list(entries())
    if args.json:
        print(json.dumps(rows, indent=2))
    else:
        print("Source audit only: modeled-partial does not imply full driver validation.")
        for group in sorted({r["namespace"] for r in rows}):
            subset = [r for r in rows if r["namespace"] == group]
            print(group, "symbols=" + str(len(subset)), dict(Counter(r["classification"] for r in subset)))
        print("Aliases are counted as symbols, not distinct implemented hardware operations.")


if __name__ == "__main__":
    main()
