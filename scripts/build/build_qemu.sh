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
cp "$lab/integrations/qemu/ubsim-simbricks-base.c" "$source_dir/hw/misc/"
cp "$lab/sources/simbricks/lib/simbricks/base/if.c" \
   "$source_dir/hw/misc/ubsim-simbricks-if-impl.c"
mkdir -p "$source_dir/include/ubsim" "$source_dir/include/hw/misc" \
         "$source_dir/include/simbricks/base"
cp "$lab/integrations/qemu/include/ubsim/ub_host_proto.h" \
   "$source_dir/include/ubsim/"
cp "$lab/integrations/qemu/include/hw/misc/ubsim-ub-host.h" \
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
    command -v docker >/dev/null || {
        echo "vmlinux is newer than Image; start the lab Docker container to regenerate it" >&2
        exit 1
    }
    container="${UBSIM_CONTAINER:-ubsim-gem5-lab}"
    container_root="${UBSIM_CONTAINER_LAB_ROOT:-/workspace/ubsim-gem5-lab}"
    docker exec "$container" aarch64-linux-gnu-objcopy \
        -O binary -R .note -R .note.gnu.build-id -R .comment -S \
        "$container_root/artifacts/kernel/vmlinux" \
        "$container_root/out/qemu-Image"
else
    cp "$lab/artifacts/kernel/Image" "$lab/out/qemu-Image"
fi
