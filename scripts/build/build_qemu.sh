#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="$(cd "$script_dir/../.." && pwd)"
version=11.1.1
archive="$lab/downloads/qemu-$version.tar.xz"
source_dir="$lab/sources/qemu-$version"
url="https://download.qemu.org/qemu-$version.tar.xz"
sha256=079ffbff8a7111bbc89022107cbabf3bbfd614d5fc9d7cc675991196aca12482
jobs="${JOBS:-4}"

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) ;;
    *) echo "build_qemu.sh currently supports Apple Silicon macOS" >&2; exit 2 ;;
esac
command -v curl >/dev/null || { echo "curl is required" >&2; exit 2; }
command -v ninja >/dev/null || { echo "ninja is required (brew install ninja)" >&2; exit 2; }

mkdir -p "$lab/downloads" "$lab/sources"
if [[ ! -f "$archive" ]]; then
    curl -L "$url" -o "$archive"
fi
actual="$(shasum -a 256 "$archive" | awk '{print $1}')"
[[ "$actual" == "$sha256" ]] || {
    echo "QEMU archive checksum mismatch: $actual" >&2
    exit 1
}
if [[ ! -d "$source_dir" ]]; then
    tar -xJf "$archive" -C "$lab/sources"
fi

patch_file="$lab/integrations/qemu/patches/qemu-$version-ubsim.patch"
if ! grep -q 'ubsim_ub_host_create' "$source_dir/hw/arm/virt.c"; then
    patch -d "$source_dir" -p1 < "$patch_file"
fi

cp "$lab/integrations/qemu/ubsim-ub-host.c" "$source_dir/hw/misc/"
cp "$lab/integrations/qemu/ubsim-tcg-sync.c" "$source_dir/hw/misc/"
cp "$lab/integrations/qemu/ubsim-simbricks-base.c" "$source_dir/hw/misc/"
cp "$lab/sources/simbricks/lib/simbricks/base/if.c" \
   "$source_dir/hw/misc/ubsim-simbricks-if-impl.c"
mkdir -p "$source_dir/include/ubsim" "$source_dir/include/hw/misc" \
         "$source_dir/include/simbricks/base"
cp "$lab/integrations/qemu/include/ubsim/ub_host_proto.h" \
   "$source_dir/include/ubsim/"
cp "$lab/integrations/qemu/include/hw/misc/ubsim-ub-host.h" \
   "$source_dir/include/hw/misc/"
cp "$lab/integrations/qemu/include/hw/misc/ubsim-tcg-sync.h" \
   "$source_dir/include/hw/misc/"
cp "$lab/sources/simbricks/lib/simbricks/base/if.h" \
   "$lab/sources/simbricks/lib/simbricks/base/proto.h" \
   "$lab/sources/simbricks/lib/simbricks/base/generic.h" \
   "$source_dir/include/simbricks/base/"

# QEMU exposes its include root with -iquote.  Keep the pinned SimBricks code
# intact in its own repository and adjust only the copied build overlay.
python3 - "$source_dir" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
files = [
    root / "hw/misc/ubsim-simbricks-if-impl.c",
    root / "include/simbricks/base/if.h",
    root / "include/simbricks/base/generic.h",
]
for path in files:
    text = path.read_text()
    text = text.replace("<simbricks/base/if.h>", '"simbricks/base/if.h"')
    text = text.replace("<simbricks/base/proto.h>", '"simbricks/base/proto.h"')
    path.write_text(text)

# The source tree may already contain an earlier UBSim patch revision.  Keep
# these small timing hooks idempotent so an incremental build gains the same
# TCG instruction-retry support as a clean extraction.
meson = root / "hw/misc/meson.build"
text = meson.read_text()
if "'ubsim-tcg-sync.c'" not in text:
    text = text.replace("  'ubsim-ub-host.c',\n",
                        "  'ubsim-ub-host.c',\n  'ubsim-tcg-sync.c',\n")
meson.write_text(text)

cputlb = root / "accel/tcg/cputlb.c"
text = cputlb.read_text()
if '"hw/misc/ubsim-tcg-sync.h"' not in text:
    text = text.replace('#include "qemu/atomic.h"\n',
                        '#include "qemu/atomic.h"\n'
                        '#include "hw/misc/ubsim-tcg-sync.h"\n')
read_call = ("        r = memory_region_dispatch_read(mr, mr_offset, &val,\n"
             "                                        this_mop, full->attrs);\n")
if "ubsim_tcg_set_retaddr(ra);\n" not in text:
    text = text.replace(read_call,
        "        ubsim_tcg_set_retaddr(ra);\n" + read_call +
        "        ubsim_tcg_set_retaddr(0);\n", 1)
write_call = ("        r = memory_region_dispatch_write(mr, mr_offset, val_le,\n"
              "                                         this_mop, full->attrs);\n")
if text.count("ubsim_tcg_set_retaddr(ra);") < 2:
    text = text.replace(write_call,
        "        ubsim_tcg_set_retaddr(ra);\n" + write_call +
        "        ubsim_tcg_set_retaddr(0);\n", 1)
cputlb.write_text(text)

# Device-completion waits are not architectural WFI. Pending IRQs must remain
# pending without defeating icount's idle warp to the completion timer. Keep
# the pinned core hooks identical for incremental and clean source builds.
def insert_once(relative, marker, anchor, replacement):
    path = root / relative
    text = path.read_text()
    if marker not in text:
        if text.count(anchor) != 1:
            raise RuntimeError(f"QEMU device-wait hook anchor mismatch: {relative}")
        path.write_text(text.replace(anchor, replacement, 1))

insert_once("include/hw/core/cpu.h", "bool ubsim_device_wait;",
            "    uint32_t halted;\n",
            "    uint32_t halted;\n"
            "    /* UBSim TCG MMIO completion wait; not interruptible WFI. */\n"
            "    bool ubsim_device_wait;\n")
insert_once("system/cpus.c", "/* UBSim device wait is idle even with pending IRQs. */",
            "    if (!cpu->halted || cpu_has_work(cpu)) {\n",
            "    /* UBSim device wait is idle even with pending IRQs. */\n"
            "    if (cpu->ubsim_device_wait) {\n"
            "        return true;\n"
            "    }\n"
            "    if (!cpu->halted || cpu_has_work(cpu)) {\n")
insert_once("system/cpus.c", "/* UBSim: only completion releases this wait. */",
            "bool cpu_can_run(CPUState *cpu)\n{\n",
            "bool cpu_can_run(CPUState *cpu)\n{\n"
            "    /* UBSim: only completion releases this wait. */\n"
            "    if (cpu->ubsim_device_wait) {\n"
            "        return false;\n"
            "    }\n")
insert_once("accel/tcg/cpu-exec.c", "/* UBSim MMIO is not an interruptible halt. */",
            "static inline bool cpu_handle_halt(CPUState *cpu)\n{\n#ifndef CONFIG_USER_ONLY\n",
            "static inline bool cpu_handle_halt(CPUState *cpu)\n{\n#ifndef CONFIG_USER_ONLY\n"
            "    /* UBSim MMIO is not an interruptible halt. */\n"
            "    if (cpu->ubsim_device_wait) {\n"
            "        return true;\n"
            "    }\n")
PY

if [[ ! -f "$source_dir/build/build.ninja" ]]; then
    (cd "$source_dir" && ./configure \
        --target-list=aarch64-softmmu --disable-docs --disable-guest-agent \
        --disable-tools --enable-hvf --enable-slirp)
fi
ninja -C "$source_dir/build" -j "$jobs" qemu-system-aarch64
"$source_dir/build/qemu-system-aarch64" --version | head -1

# QEMU direct boot needs the flat ARM64 Image.  A normal kernel build already
# keeps Image and vmlinux together; regenerate only when an incremental module
# build has made vmlinux newer than the copied Image.
mkdir -p "$lab/out"
if [[ "$lab/artifacts/kernel/vmlinux" -nt "$lab/artifacts/kernel/Image" ]]; then
    if [[ "$lab/out/qemu-Image" -nt "$lab/artifacts/kernel/vmlinux" ]]; then
        : # Preserve a flat image already generated from this vmlinux.
    else
        python3 "$lab/tools/elf_to_flat_binary.py" \
            "$lab/artifacts/kernel/vmlinux" "$lab/out/qemu-Image"
    fi
else
    cp "$lab/artifacts/kernel/Image" "$lab/out/qemu-Image"
fi
