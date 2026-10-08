#!/usr/bin/env python3
"""Build an isolated ARM64 reporting diagnostic and replace only its initramfs entry."""
import gzip
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
PIN = "8f272493e4138cd52cfb3ce11064a07c8d1be49f"
REL = "src/urma/tools/urma_perftest/perftest_run_test.c"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(argv, **kwargs):
    return subprocess.run(argv, check=True, **kwargs)


def replace_perftest(raw, replacement):
    output = bytearray()
    offset = 0
    replaced = []
    unchanged = 0
    while offset < len(raw):
        header_offset = offset
        if raw[offset:offset+6] != b"070701":
            if not raw[offset:].strip(b"\0"):
                break
            raise RuntimeError("unsupported initramfs format")
        hdr = raw[offset:offset+110]
        size = int(hdr[54:62], 16)
        namesize = int(hdr[94:102], 16)
        name = raw[offset+110:offset+110+namesize-1].decode()
        content_offset = (offset + 110 + namesize + 3) & ~3
        end = (content_offset + size + 3) & ~3
        if name.removeprefix("./") == "usr/bin/urma_perftest":
            replaced.append(dict(name=name, old_sha256=sha(raw[content_offset:content_offset+size]),
                                 new_sha256=sha(replacement)))
            output.extend(hdr[:54] + f"{len(replacement):08x}".encode() + hdr[62:])
            output.extend(raw[header_offset+110:content_offset])
            output.extend(replacement)
            output.extend(b"\0" * (-len(replacement) % 4))
        else:
            output.extend(raw[header_offset:end])
            unchanged += 1
        offset = end
        if name == "TRAILER!!!":
            break
    if len(replaced) != 1:
        raise RuntimeError("expected exactly one perftest entry")
    output.extend(b"\0" * (-len(output) % 512))
    return bytes(output), replaced, unchanged


def main():
    start = time.monotonic()
    build = ROOT / "artifacts/perftest-bw-diagnostic-build"
    image = ROOT / "out/official-udma-bw-diagnostic.cpio.gz"
    if image.exists():
        raise RuntimeError("diagnostic image already exists; refusing overwrite")
    build.mkdir(exist_ok=False)
    source = build / "source" / REL
    source.parent.mkdir(parents=True)
    source.write_bytes(run(["git", "show", f"{PIN}:{REL}"], cwd=ROOT / "sources/umdk",
                          stdout=subprocess.PIPE).stdout)
    patch = ROOT / "patches/source/umdk/0011-add-opt-in-bandwidth-report-diagnostic.patch"
    run(["git", "init", "-q"], cwd=build / "source")
    run(["git", "apply", str(patch)], cwd=build / "source")
    if "UBSIM_BW_REPORT_DIAGNOSTIC" not in source.read_text():
        raise RuntimeError("diagnostic overlay was not applied")
    header = run(["git", "show", "fd4d120^:tools/ou-m5ops.h"], cwd=ROOT,
                 stdout=subprocess.PIPE).stdout
    (build / "ou-m5ops.h").write_bytes(header)
    target = ROOT / "artifacts/umdk-build/urma/tools/urma_perftest"
    cmake = target / "CMakeFiles/urma_perftest.dir"
    values = {}
    for line in (cmake / "flags.make").read_text().splitlines():
        if " = " in line:
            key, value = line.split(" = ", 1)
            values[key] = shlex.split(value)
    flags = values["C_DEFINES"] + values["C_INCLUDES"] + values["C_FLAGS"]
    old = "/workspace/openurma-gem5-lab/sources/OpenURMA/integration/umdk/vendor/umdk"
    flags = [flag.replace(old, str(ROOT / "sources/umdk")) for flag in flags]
    flags += [f"-I{build}", f"-I{ROOT / 'sources/umdk/src/urma/tools/urma_perftest'}"]
    begin_compile = time.monotonic()
    obj = build / "perftest_run_test.c.o"
    run(["gcc", *flags, "-c", str(source), "-o", str(obj)])
    link = shlex.split((cmake / "link.txt").read_text())
    link[link.index("CMakeFiles/urma_perftest.dir/perftest_run_test.c.o")] = str(obj)
    binary = build / "urma_perftest"
    link[link.index("-o") + 1] = str(binary)
    run(link, cwd=target)
    compile_wall = time.monotonic() - begin_compile
    base = ROOT / "out/official-udma.cpio.gz"
    base_bytes = base.read_bytes()
    raw = gzip.decompress(base_bytes)
    replacement = binary.read_bytes()
    if b"UBSIM_BW_REPORT_DIAGNOSTIC" not in replacement:
        raise RuntimeError("diagnostic marker missing from ELF")
    output, replaced, unchanged = replace_perftest(raw, replacement)
    image.write_bytes(gzip.compress(bytes(output), mtime=0))
    manifest = dict(source_commit=PIN, patch_sha256=sha(patch.read_bytes()),
                    source_sha256=sha(source.read_bytes()), header_sha256=sha(header),
                    header_revision="fd4d120^:tools/ou-m5ops.h", compile_command=["gcc", *flags],
                    link_command=link, base_image=str(base), base_sha256=sha(base_bytes),
                    image=str(image), image_sha256=sha(image.read_bytes()),
                    replacement=replaced, unchanged_cpio_records=unchanged,
                    compile_link_wall_seconds=compile_wall,
                    total_wall_seconds=time.monotonic()-start)
    (image.with_suffix("").with_suffix(".manifest.json")).write_text(json.dumps(manifest, indent=2)+"\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
