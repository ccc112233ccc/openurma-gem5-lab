#!/usr/bin/env python3
"""Make unmodified UMDK cross-compile correctly on an x86_64 host.

The pinned upstream UMDK CMake files select architecture flags from
CMAKE_HOST_SYSTEM_PROCESSOR.  During a real x86_64 -> AArch64 cross build this
incorrectly adds x86 SSE flags and UB_ARCH_X86_64.  This compiler launcher
removes only those host-derived flags, adds the corresponding ARM64 flags, and
redirects the one absolute libnl include path into the target sysroot.
"""

from __future__ import annotations

import os
from pathlib import Path
import sys


def main() -> int:
    sysroot_value = os.environ.get("OPENURMA_ARM64_SYSROOT", "")
    if not sysroot_value:
        print("aarch64-umdk-cc: OPENURMA_ARM64_SYSROOT is required", file=sys.stderr)
        return 2
    sysroot = Path(sysroot_value).resolve()
    if not (sysroot / "usr/include").is_dir():
        print(f"aarch64-umdk-cc: invalid ARM64 sysroot: {sysroot}", file=sys.stderr)
        return 2

    real_cc = os.environ.get("OPENURMA_AARCH64_CC", "aarch64-linux-gnu-gcc")
    args: list[str] = []
    for arg in sys.argv[1:]:
        if arg in {"-msse4.2", "-DUB_ARCH_X86_64"}:
            continue
        if arg == "-I/usr/include/libnl3":
            arg = f"-I{sysroot}/usr/include/libnl3"
        args.append(arg)

    # Supplying the sysroot through the launcher also covers CMake's compiler
    # probes, which happen before project-level flags are fully established.
    args.extend((f"--sysroot={sysroot}", "-march=armv8-a+crc", "-DUB_ARCH_ARM64"))
    os.execvp(real_cc, [real_cc, *args])
    return 127


if __name__ == "__main__":
    raise SystemExit(main())
