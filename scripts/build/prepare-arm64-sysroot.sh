#!/usr/bin/env bash
# Prepare an ARM64 Ubuntu userspace/sysroot without executing ARM instructions.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="${OPENURMA_LAB_ROOT:-$(cd "$script_dir/../.." && pwd)}"
sysroot="${OPENURMA_ARM64_SYSROOT:-$lab/artifacts/sysroots/ubuntu-22.04-arm64}"
mirror="${OPENURMA_UBUNTU_PORTS_MIRROR:-http://ports.ubuntu.com/ubuntu-ports}"
marker="$sysroot/.openurma-sysroot-v1"

die() { echo "prepare-arm64-sysroot.sh: $*" >&2; exit 2; }

[[ "$(uname -s)" == Linux ]] || die "Linux is required"
[[ "$(uname -m)" == x86_64 ]] || die "this sysroot preparation path is for x86_64 Linux"
command -v debootstrap >/dev/null || die "debootstrap is required"
[[ -n "$sysroot" && "$sysroot" != / ]] || die "unsafe sysroot path: $sysroot"

if [[ -f "$marker" && -x "$sysroot/bin/busybox" && -d "$sysroot/usr/include/libnl3" ]]; then
    echo "[arm64-sysroot] already ready: $sysroot"
    exit 0
fi

[[ ! -e "$sysroot" || -z "$(find "$sysroot" -mindepth 1 -maxdepth 1 -print -quit)" ]] || \
    die "$sysroot is incomplete and non-empty; move it aside and retry"
mkdir -p "$sysroot"

# --foreign performs only download/extraction.  It does not chroot or execute
# target binaries, so qemu-user and an ARM64 container are not required.
debootstrap --arch=arm64 --foreign --variant=minbase --components=main,universe \
    --include=busybox-static,libnl-3-dev,libnl-genl-3-dev,libnl-route-3-dev,libssl-dev \
    jammy "$sysroot" "$mirror"

[[ -x "$sysroot/bin/busybox" ]] || die "ARM64 static BusyBox was not unpacked"
[[ -d "$sysroot/usr/include/libnl3" ]] || die "ARM64 libnl headers were not unpacked"
printf '%s\n' 'Ubuntu 22.04 arm64 foreign-stage sysroot' > "$marker"
echo "[arm64-sysroot] ready: $sysroot"
