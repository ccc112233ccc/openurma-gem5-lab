#!/usr/bin/env python3
"""Convert a little-endian ELF64 kernel image to objcopy-style flat binary."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
SECTION_HEADER = struct.Struct("<IIQQQQIIQQ")
SHF_ALLOC = 0x2
SHT_NOBITS = 8


def convert(source: Path, destination: Path) -> None:
    data = source.read_bytes()
    header = ELF_HEADER.unpack_from(data)
    ident = header[0]
    if ident[:6] != b"\x7fELF\x02\x01":
        raise ValueError(f"{source} is not a little-endian ELF64 file")
    section_offset, section_size, section_count = header[6], header[11], header[12]
    sections = [
        SECTION_HEADER.unpack_from(data, section_offset + index * section_size)
        for index in range(section_count)
    ]
    loadable = [
        section for section in sections
        if section[2] & SHF_ALLOC and section[1] != SHT_NOBITS and section[5]
    ]
    if not loadable:
        raise ValueError(f"{source} has no loadable initialized sections")
    base = min(section[3] for section in loadable)
    end = max(section[3] + section[5] for section in loadable)
    image = bytearray(end - base)
    for section in loadable:
        address, offset, size = section[3], section[4], section[5]
        image[address - base:address - base + size] = data[offset:offset + size]
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(image)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    convert(args.source, args.destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
