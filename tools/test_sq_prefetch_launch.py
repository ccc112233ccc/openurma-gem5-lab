"""Bounded SQ prefetch settings must reach both host-simulator launch paths."""
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
KEYS = ("sq_prefetch_wqebbs", "sq_fetch_burst_wqebbs", "sq_fetch_max_outstanding")


class SqPrefetchLaunchTest(unittest.TestCase):
    def test_wiring_and_boundaries(self):
        for name in ("run-qemu-dual.sh", "run-dual.sh"):
            source = (ROOT / "scripts/run" / name).read_text()
            block = re.search(r'for value in "\$sq_prefetch_wqebbs".*?\ndone', source, re.S).group()
            for key in KEYS:
                self.assertIn(f'--{key.replace("_", "-")} "${key}"', source)
                self.assertIn(f'{key}=${key}\n', source)
                for value, valid in (("1", True), ("4096", True), ("0", False),
                                     ("4097", False), ("-1", False), ("no", False)):
                    with self.subTest(launcher=name, key=key, value=value):
                        code = "\n".join(f'{k}={value if k == key else 1}' for k in KEYS)
                        result = subprocess.run(["bash", "-c", code +
                            '\ndie() { exit 23; }\n' + block], capture_output=True)
                        self.assertEqual(result.returncode, 0 if valid else 23)

    def test_qemu_manifest_identifies_launched_artifacts(self):
        source = (ROOT / "scripts/run/run-qemu-dual.sh").read_text()
        for key in ("qemu_binary", "udma_device_binary", "ns3_binary", "kernel_image", "initrd_image"):
            self.assertRegex(source, rf'(?m)^{key}=\$\w+$')


if __name__ == "__main__":
    unittest.main()
