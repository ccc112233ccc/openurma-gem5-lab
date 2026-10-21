#!/usr/bin/env python3
"""Normalize gem5 vector<bool> text so its bool parser can restore it."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import stat
import tempfile


def normalize(path: Path) -> bool:
    section = ""
    changed = False
    source_mode = stat.S_IMODE(path.stat().st_mode)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile("w", dir=path.parent, prefix=".m5.cpt.",
                                         delete=False, encoding="utf-8") as output:
            temporary = Path(output.name)
            with path.open(encoding="utf-8") as source:
                for line in source:
                    if line.startswith("["):
                        section = line.strip()
                    if section == "[system.realview.flash1.blocks]" and \
                            line.startswith("locked="):
                        values = line.removeprefix("locked=").split()
                        if values and set(values) <= {"0", "1"}:
                            line = "locked=" + " ".join(
                                "true" if value == "1" else "false"
                                for value in values) + "\n"
                            changed = True
                        elif values and not set(values) <= {"true", "false"}:
                            raise ValueError(f"invalid flash lock vector in {path}")
                    output.write(line)
        if changed:
            os.chmod(temporary, source_mode)
            os.replace(temporary, path)
        else:
            temporary.unlink()
        return changed
    except Exception:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("checkpoints", type=Path, nargs="+")
    args = parser.parse_args()
    for path in args.checkpoints:
        print(f"{path}: {'normalized' if normalize(path) else 'already compatible'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
