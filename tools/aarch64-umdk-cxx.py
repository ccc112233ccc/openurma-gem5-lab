#!/usr/bin/env python3
"""C++ companion to aarch64-umdk-cc.py for ARM64 cross builds."""

from __future__ import annotations

import os
from pathlib import Path
import sys


def main() -> int:
    sysroot_value = os.environ.get("OPENURMA_ARM64_SYSROOT", "")
    if not sysroot_value:
        print("aarch64-umdk-cxx: OPENURMA_ARM64_SYSROOT is required", file=sys.stderr)
        return 2
    sysroot = Path(sysroot_value).resolve()
    if not (sysroot / "usr/include").is_dir():
        print(f"aarch64-umdk-cxx: invalid ARM64 sysroot: {sysroot}", file=sys.stderr)
        return 2
    real_cxx = os.environ.get("OPENURMA_AARCH64_CXX", "aarch64-linux-gnu-g++")
    args = []
    for arg in sys.argv[1:]:
        if arg in {"-msse4.2", "-DUB_ARCH_X86_64"}:
            continue
        if arg == "-I/usr/include/libnl3":
            arg = f"-I{sysroot}/usr/include/libnl3"
        args.append(arg)
    args.extend((f"--sysroot={sysroot}", "-march=armv8-a+crc", "-DUB_ARCH_ARM64"))
    os.execvp(real_cxx, [real_cxx, *args])
    return 127


if __name__ == "__main__":
    raise SystemExit(main())
