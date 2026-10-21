#!/usr/bin/env python3
"""Build private UDMA provider variants for controlled hot-path comparisons."""

from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "sources/umdk/src/urma/hw/udma"
TARGET = ROOT / "artifacts/umdk-build/urma/hw/udma"
OUTPUT = ROOT / "artifacts/udma-hotpath-probes"
NAMES = ("udma_u_jfs.c", "udma_u_jfc.c")


def build(name: str, *, no_trapv: bool = False,
          prefetch_distance: int = 0, prefetch_locality: int = 1,
          sqe_prefetch_distance: int = 0, sqe_prefetch_locality: int = 1,
          no_cqe_barrier: bool = False,
          optimize_o3: bool = False) -> Path:
    directory = OUTPUT / name
    directory.mkdir(parents=True, exist_ok=True)
    cmake = TARGET / "CMakeFiles/BETA_UDMA_SO.dir"
    values = dict(line.split(" = ", 1) for line in
                  (cmake / "flags.make").read_text().splitlines()
                  if " = " in line)
    flags = sum((shlex.split(values[key]) for key in
                 ("C_DEFINES", "C_INCLUDES", "C_FLAGS")), [])
    flags += [f"-I{SOURCE}", f"-I{ROOT / 'deps/ummu/include'}",
              f"-I{ROOT / 'deps/ummu/kernel_headers'}"]
    if no_trapv:
        flags.append("-fno-trapv")
    if optimize_o3:
        flags.append("-O3")
    replacements = {}
    for source_name in NAMES:
        original = SOURCE / source_name
        source = directory / source_name
        contents = original.read_text()
        if prefetch_distance and source_name == "udma_u_jfc.c":
            old = "\tstruct udma_u_jfc_cqe *cqe = get_next_cqe(udma_u_jfc, udma_u_jfc->cq.ci);"
            new = (f"\t/* Diagnostic: fetch {prefetch_distance} later CQE cache lines. */\n"
                   "\t__builtin_prefetch(get_u_buf_entry(&udma_u_jfc->cq, "
                   f"udma_u_jfc->cq.ci + {prefetch_distance}), 0, "
                   f"{prefetch_locality});\n" + old)
            if contents.count(old) != 1:
                raise RuntimeError("CQE lookup marker not unique")
            contents = contents.replace(old, new)
        if no_cqe_barrier and source_name == "udma_u_jfc.c":
            old = "\tUDMA_FROM_DEVICE_BARRIER();\n\t++udma_u_jfc->cq.ci;"
            if contents.count(old) != 1:
                raise RuntimeError("CQE read barrier marker not unique")
            contents = contents.replace(old,
                "\t/* Diagnostic only: no CQE read barrier. */\n\t++udma_u_jfc->cq.ci;")
        if sqe_prefetch_distance and source_name == "udma_u_jfs.c":
            old = ("\tret = udma_set_sqe((struct udma_jfs_sqe_ctl *)"
                   "sq->qbuf_curr, sq, wr, &wqe_info);")
            new = ("\t__builtin_prefetch(udma_inc_ptr_wrap("
                   "(uint8_t *)sq->qbuf_curr, "
                   f"{sqe_prefetch_distance}u << sq->baseblk_shift, "
                   f"(uint8_t *)sq->qbuf, (uint8_t *)sq->qbuf_end), 1, {sqe_prefetch_locality});\n"
                   + old)
            if contents.count(old) != 1:
                raise RuntimeError("SQE encode marker not unique")
            contents = contents.replace(old, new)
        source.write_text(contents)
        obj = directory / (source_name + ".o")
        subprocess.run(["gcc", *flags, "-c", str(source), "-o", str(obj)],
                       check=True)
        replacements[f"CMakeFiles/BETA_UDMA_SO.dir/{source_name}.o"] = str(obj)
    link = shlex.split((cmake / "link.txt").read_text())
    for original, replacement in replacements.items():
        link[link.index(original)] = replacement
    library = directory / "liburma-udma.so"
    link[link.index("-o") + 1] = str(library)
    link.insert(link.index("-lummu"),
                f"-L{ROOT / 'artifacts/umdk-build-ummu-official'}")
    subprocess.run(link, cwd=TARGET, check=True)
    print(name, library, library.stat().st_size, flush=True)
    return library


def main() -> None:
    build("relinked")
    build("no-trapv", no_trapv=True)
    build("cqe-prefetch", prefetch_distance=4)
    build("cqe-prefetch8", prefetch_distance=8)
    build("cqe-prefetch16", prefetch_distance=16)
    build("cqe-prefetch4-l1", prefetch_distance=4, prefetch_locality=3)
    build("sqe-prefetch4", sqe_prefetch_distance=4)
    build("cqe4-sqe4", prefetch_distance=4, sqe_prefetch_distance=4)
    build("no-cqe-barrier", no_cqe_barrier=True)
    build("cqe4-sqe4-o3", prefetch_distance=4,
          sqe_prefetch_distance=4, optimize_o3=True)
    build("o3-only", optimize_o3=True)


if __name__ == "__main__":
    main()
