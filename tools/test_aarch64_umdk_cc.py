#!/usr/bin/env python3
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent


class Aarch64UmdkCompilerTest(unittest.TestCase):
    def test_replaces_upstream_host_architecture_flags(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "sysroot/usr/include/libnl3").mkdir(parents=True)
            compiler = root / "compiler"
            compiler.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\"\n", encoding="utf-8")
            compiler.chmod(0o755)
            env = os.environ.copy()
            env["OPENURMA_ARM64_SYSROOT"] = str(root / "sysroot")
            env["OPENURMA_AARCH64_CC"] = str(compiler)
            result = subprocess.run(
                [
                    str(ROOT / "tools/aarch64-umdk-cc.py"),
                    "-msse4.2",
                    "-DUB_ARCH_X86_64",
                    "-I/usr/include/libnl3",
                    "-c",
                    "probe.c",
                ],
                env=env,
                check=True,
                capture_output=True,
                text=True,
            )
            args = result.stdout.splitlines()
            self.assertNotIn("-msse4.2", args)
            self.assertNotIn("-DUB_ARCH_X86_64", args)
            self.assertIn("-DUB_ARCH_ARM64", args)
            self.assertIn("-march=armv8-a+crc", args)
            sysroot = (root / "sysroot").resolve()
            self.assertIn(f"--sysroot={sysroot}", args)
            self.assertIn(f"-I{sysroot / 'usr/include/libnl3'}", args)


if __name__ == "__main__":
    unittest.main()
