#!/usr/bin/env python3
"""Run with: python3 -m unittest discover -s tools -p 'test_audit_command_coverage.py'."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import audit_command_coverage as audit


class EnumParsingTest(unittest.TestCase):
    def test_comments_aliases_suffixes_and_increment(self):
        source = """enum sample { A = 0x1U, // comment
            B, C = (A << 3) | 2, D = C, /* comma, */ E = D + 1, };"""
        self.assertEqual(list(audit.enum_entries(source, "sample")),
                         [("A", 1), ("B", 2), ("C", 10), ("D", 10), ("E", 11)])

    def test_fail_closed(self):
        for source in ["enum other { A };", "enum x { A }; enum x { B };",
                       "enum x { A, A };", "enum x { A = MACRO };",
                       "enum x { A,,B };", "enum x { A = 0x10000 };",
                       "enum x { A = -1 };", "enum x { A = function() };",
                       "enum x { A = 1 << 100 };", "enum x { #if FOO\nA\n#endif };",
                       "enum x { A = };", "enum x { A = 1 / 0 };"]:
            with self.subTest(source=source), self.assertRaises(ValueError):
                list(audit.enum_entries(source, "x"))

    def test_ambiguous_anonymous_enum(self):
        with self.assertRaises(ValueError):
            list(audit.enum_entries("enum { A }; enum { B };", ""))


class InventoryTest(unittest.TestCase):
    def test_dedup_aliases_and_namespace_isolation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "a.h").write_text("enum a { Z = 1, A = 1, A_MAX = 2 };")
            (root / "b.h").write_text("enum b { B = 1 };")
            groups = [("ubase", "a.h", "a"), ("ubase", "b.h", "b"),
                      ("ctrlq-tp", "b.h", "b")]
            rows = list(audit.entries(root, groups))
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows, list(audit.entries(root, list(reversed(groups)))))
            ubase = next(row for row in rows if row["namespace"] == "ubase")
            self.assertEqual([alias["symbol"] for alias in ubase["aliases"]], ["A", "B", "Z"])
            self.assertEqual(ubase["execution_evidence"], [])
            self.assertEqual(ubase["classification_basis"], "source-inventory-drift")
            self.assertEqual(ubase["classification"], "unreviewed")

    def test_pinned_inventory_covers_ctrlq_services(self):
        rows = list(audit.entries())
        keys = [(row["namespace"], row["opcode"]) for row in rows]
        self.assertEqual(len(keys), len(set(keys)))
        self.assertEqual({row["service_id"] for row in rows if row["service_id"]},
                         {1, 2, 3, 4, 5})
        self.assertTrue(all(not row["execution_evidence"] for row in rows))
        self.assertEqual(next(row["classification"] for row in rows
                              if (row["namespace"], row["opcode"]) == ("ctrlq-ip", "0x1")),
                         "rejected")
        tp = next(row for row in rows if (row["namespace"], row["opcode"]) == ("ctrlq-tp", "0x14"))
        self.assertEqual(len(tp["aliases"]), 2)

    def test_json_contract_and_repeatability(self):
        command = [sys.executable, str(Path(audit.__file__)), "--json", "--check"]
        first = subprocess.check_output(command)
        self.assertEqual(first, subprocess.check_output(command))
        result = json.loads(first)
        self.assertEqual(result["schema_version"], 1)
        self.assertFalse(result["execution_evidence_collected"])
        self.assertTrue(result["commands"])

    def test_new_alias_invalidates_review(self):
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "a.h"
            source.write_text("enum a { A = 1 };")
            groups = [("ubase", "a.h", "a")]
            baseline = audit.declaration_fingerprints(list(audit.entries(root, groups)))
            with patch.object(audit, "REVIEWED_DECLARATIONS", baseline):
                self.assertEqual(list(audit.entries(root, groups))[0]["classification"], "modeled-partial")
                source.write_text("enum a { A = 1, NEW_ALIAS = 1 };")
                self.assertEqual(list(audit.entries(root, groups))[0]["classification"], "unreviewed")


if __name__ == "__main__":
    unittest.main()
