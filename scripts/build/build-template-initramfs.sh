#!/usr/bin/env bash
# Repackage a retained 64-KiB guest on a host without an ARM64 cross compiler.
# The normal Linux initramfs builder compiles ubsim-readfile from source.
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="${UBSIM_LAB_ROOT:-$(cd "$script_dir/../.." && pwd)}"
source_image="${UBSIM_SOURCE_INITRD:-$lab/out/official-udma-64k-20261015.cpio.gz}"
readfile_binary="${UBSIM_READFILE_BINARY:-$lab/artifacts/urma-phase-profile/ubsim-readfile}"
output="${OUT:-$lab/out/official-udma-64k-node-template-20261021.cpio.gz}"
die() { echo "build-template-initramfs.sh: $*" >&2; exit 2; }
[[ -f "$source_image" ]] || die "missing source image: $source_image"
[[ -f "$readfile_binary" ]] || die "missing ARM64 ubsim-readfile: $readfile_binary"
[[ -f "$lab/overlay/init" ]] || die "missing guest init"
[[ ! -e "$output" ]] || die "output already exists; choose a new OUT so retained checkpoint hashes stay valid: $output"
file "$readfile_binary" | grep -Eq 'ARM aarch64|aarch64' ||
    die "readfile helper is not an AArch64 ELF executable"

stage=$(mktemp -d "${TMPDIR:-/tmp}/ubsim-template-image.XXXXXX")
temporary="$output.tmp.$$"
trap 'rm -rf "$stage" "$temporary"' EXIT
(
    cd "$stage"
    gzip -dc "$source_image" | cpio -idm --quiet
    cp "$lab/overlay/init" ./init
    cp "$readfile_binary" ./usr/bin/ubsim-readfile
    chmod 0755 ./init ./usr/bin/ubsim-readfile
    find . -print0 | LC_ALL=C sort -z |
        cpio --null -o --format=newc --owner=0:0 --quiet | gzip -n -9 > "$temporary"
)
mv "$temporary" "$output"
printf 'template image: %s\nsha256: ' "$output"
sha256sum "$output" | awk '{print $1}'
