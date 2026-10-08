"""Every patched upstream file must be restored before a repeat adapter build."""
from pathlib import Path
import re
import shlex
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PatchManifestTest(unittest.TestCase):
    def test_restore_covers_patch_series(self):
        script = (ROOT / "scripts/build-ns3ub-adapter.sh").read_text().replace("\\\n", " ")
        restore = next(line for line in script.splitlines()
                       if line.startswith('ubsim_exec git -C "$source_root" restore '))
        tokens = shlex.split(restore)
        restored = set(tokens[tokens.index("--") + 1:])
        patched = set()
        for patch in sorted((ROOT / "integrations/ns3ub/patches").glob("*.patch")):
            self.assertIn(patch.name, script, f"unwired patch: {patch.name}")
            patched.update(re.findall(r"(?m)^\+\+\+ b/(\S+)", patch.read_text()))
        self.assertTrue(patched)
        self.assertEqual(patched - restored, set(), "repeat build would reapply onto already patched files")


if __name__ == "__main__":
    unittest.main()
