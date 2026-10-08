#!/usr/bin/env python3
"""Inventory pinned driver enums against reviewed model behavior (not execution proof)."""
import argparse
import ast
import hashlib
from collections import Counter
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
UDMA = "oe66/drivers/ub/urma/hw/udma/udma_cmd.h"
CTRLQ = "oe66/include/ub/ubase/ubase_comm_ctrlq.h"
GROUPS = [
    ("ubase", "oe66/include/ub/ubase/ubase_comm_cmd.h", "ubase_opcode_type"),
    ("ubase", UDMA, "udma_cmd_opcode_type"),
    ("mailbox", UDMA, ""),
    ("mailbox", "oe66/include/ub/ubase/ubase_comm_mbx.h", "ubase_mbox_opcode"),
    ("ctrlq-tp", "oe66/drivers/ub/urma/hw/udma/udma_ctrlq_tp.h", "udma_ctrlq_cmd_code_type"),
    ("ctrlq-device", UDMA, "udma_ctrlq_dev_mgmt_opcode"),
    ("ctrlq-tp", CTRLQ, "ubase_ctrlq_opc_type_tp"),
    ("ctrlq-device", CTRLQ, "ubase_ctrlq_opc_type_dev_register"),
    ("ctrlq-qos", CTRLQ, "ubase_ctrlq_opc_type_qos"),
    ("ctrlq-ip", CTRLQ, "ubase_ctrlq_opc_type_ip"),
    ("ctrlq-ubmem", "oe66/drivers/ub/urma/hw/udma/udma_ctrlq_tp.h", "udma_ctrlq_ubmem_opcode"),
]
SERVICE_IDS = {"ctrlq-tp": 1, "ctrlq-device": 2, "ctrlq-ip": 3,
               "ctrlq-qos": 4, "ctrlq-ubmem": 5}
# Manually reviewed declaration baselines, NOT hashes of implementation support.
# On pinned source changes, review every changed declaration before updating.
# Namespace-wide invalidation deliberately fails closed even for new aliases.
REVIEWED_DECLARATIONS = {
    "ctrlq-device": "5766d71c1446300e07437b05816b129bdd7cba46985c772a76e06645ac842e0a",
    "ctrlq-ip": "5ac517ef2390465763a6a0b903376da30e94f33f89d2647c5a0626e6cb4b5868",
    "ctrlq-qos": "fdc6e6af9518ef61ae9c7fff029b635c2b08792058830bbab66d0439cf1e43ce",
    "ctrlq-tp": "717cc73a731db23832e071bff960cc26d35e623534fa5fdef80b07d0b2dac0cf",
    "ctrlq-ubmem": "f249ccd1be89d699701f7ad7fb4deed1dce26fccc2418955abe03cf4e91629a1",
    "mailbox": "9fcb8182b8a0a3a28b30ce294e2342d7b56bc2d351b3e4e55827ac25a0c26841",
    "ubase": "3bd32e5a954a156791c1a1198b54ff7057ea6113c60707fc413cb3f2fbec5c8e",
}
# These classifications are deliberately conservative and reviewed by hand.
# A modeled command can still have partial semantics; see the coverage document.
MODELED = {
    "ubase": {0x1, 0x2, 0x30, 0x6200, 0x7001},
    "mailbox": {0x4, 0x6, 0x7, 0x24, 0x25, 0x26, 0x27, 0x34, 0x37, 0x44, 0x47, 0x54, 0x57},
    "ctrlq-tp": {0x13, 0x21, 0x22, 0x23},
    "ctrlq-device": {0x1, 0x15},
    "ctrlq-qos": {0x1, 0x2},
    "ctrlq-ip": set(),
    "ctrlq-ubmem": set(),
}


def enum_entries(text, enum_name, source="<text>"):
    """Parse the pinned enum subset, failing closed on ambiguity/new syntax.

    This is not a C preprocessor. Conditional enum bodies and external macros
    are deliberately rejected, so source changes cannot silently corrupt counts.
    """
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    pattern = (r"\benum\s+" + re.escape(enum_name) + r"\s*\{([^}]+)\}"
               if enum_name else r"\benum\s*\{([^}]+)\}")
    matches = re.findall(pattern, text, re.S)
    if len(matches) != 1:
        raise ValueError(f"{source}: expected exactly one enum {enum_name!r}, found {len(matches)}")
    values = {}
    value = -1

    def evaluate(expression):
        expression = re.sub(r"\b(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]+\b", r"\1", expression)
        def visit(node):
            if isinstance(node, ast.Constant) and type(node.value) is int:
                return node.value
            if isinstance(node, ast.Name) and node.id in values:
                return values[node.id]
            if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub)):
                return visit(node.operand) * (-1 if isinstance(node.op, ast.USub) else 1)
            if isinstance(node, ast.BinOp):
                left, right = visit(node.left), visit(node.right)
                if isinstance(node.op, ast.Add): return left + right
                if isinstance(node.op, ast.Sub): return left - right
                if isinstance(node.op, ast.BitOr): return left | right
                if isinstance(node.op, ast.LShift) and 0 <= right <= 63: return left << right
            raise ValueError(f"unsupported enum expression {expression!r}")
        return visit(ast.parse(expression, mode="eval").body)

    for index, item in enumerate(matches[0].split(",")):
        item = item.strip()
        if not item and index == len(matches[0].split(",")) - 1:
            continue
        parts = item.split("=", 1)
        symbol = parts[0].strip()
        if not re.fullmatch(r"[A-Za-z_]\w*", symbol) or symbol in values:
            raise ValueError(f"{source}: invalid/duplicate enum symbol {symbol!r}")
        try:
            value = evaluate(parts[1].strip()) if len(parts) == 2 else value + 1
        except (ValueError, SyntaxError) as error:
            raise ValueError(f"{source}: {symbol}: {error}") from error
        if not 0 <= value <= 0xffff:
            raise ValueError(f"{source}: opcode out of range: {symbol}={value}")
        values[symbol] = value
        yield symbol, value


def entries(root=ROOT, groups=GROUPS):
    """One record per wire namespace/opcode; retain every declaration alias."""
    rows = {}
    for namespace, path, enum_name in groups:
        for symbol, value in enum_entries((root / path).read_text(), enum_name, path):
            if symbol.endswith("_MAX"):
                continue
            if namespace == "ubase" and value in {0x7000, 0xf00e}:
                status = "dispatch-only"
            elif namespace == "ubase" and value in {0x4213, 0x4214}:
                status = "rejected"
            elif value in MODELED.get(namespace, set()):
                status = "modeled-partial"
            elif namespace.startswith("ctrlq"):
                status = "rejected"
            else:
                status = "completion-only"
            key = namespace, value
            row = rows.setdefault(key, {
                "namespace": namespace, "opcode": hex(value),
                "service_id": SERVICE_IDS.get(namespace),
                "classification": status,
                "classification_basis": "manual-source-review",
                "execution_evidence": [],
                "aliases": [],
            })
            alias = {"symbol": symbol, "source": path, "enum": enum_name}
            if alias not in row["aliases"]:
                row["aliases"].append(alias)
    for key in sorted(rows):
        rows[key]["aliases"].sort(key=lambda alias: (alias["symbol"], alias["source"]))
    fingerprints = declaration_fingerprints(list(rows.values()))
    for key in sorted(rows):
        if fingerprints[key[0]] != REVIEWED_DECLARATIONS.get(key[0]):
            rows[key]["classification"] = "unreviewed"
            rows[key]["classification_basis"] = "source-inventory-drift"
        yield rows[key]


def declaration_fingerprints(rows):
    return {
        namespace: hashlib.sha256(json.dumps(
            [[row["opcode"], row["aliases"]] for row in sorted(
                rows, key=lambda row: (row["namespace"], int(row["opcode"], 16)))
             if row["namespace"] == namespace],
            sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        for namespace in sorted({row["namespace"] for row in rows})
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="Print versioned distinct-opcode inventory with aliases")
    parser.add_argument("--check", action="store_true", help="Fail if declarations changed without manual review")
    args = parser.parse_args()
    rows = list(entries())
    if args.json:
        print(json.dumps({"schema_version": 1, "scope": "selected-pinned-driver-enums",
                          "execution_evidence_collected": False, "commands": rows}, indent=2))
    else:
        print("Source audit only: modeled-partial does not imply full driver validation.")
        for group in sorted({r["namespace"] for r in rows}):
            subset = [r for r in rows if r["namespace"] == group]
            print(group, "opcodes=" + str(len(subset)),
                  "declarations=" + str(sum(len(r["aliases"]) for r in subset)),
                  dict(sorted(Counter(r["classification"] for r in subset).items())))
        print("Counts are distinct namespace/opcode pairs; aliases are declarations, not extra operations.")
        print("All classifications are manual source review; execution evidence is not collected.")
    if args.check and declaration_fingerprints(rows) != REVIEWED_DECLARATIONS:
        print("Declaration inventory changed: manual review required; no support inference is valid.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
