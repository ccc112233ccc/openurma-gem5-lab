#!/usr/bin/env bash
# Build every artifact required by run-dual.sh from the fetched source trees.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="${OPENURMA_LAB_ROOT:-$(cd "$script_dir/.." && pwd)}"
openurma="$lab/sources/OpenURMA"
openclicknp="$lab/sources/OpenClickNP"
kernel_root="${KSRC:-$lab/oe66}"
jobs="${JOBS:-2}"
build_mode="${OPENURMA_BUILD_MODE:-native}"
arm64_sysroot="${OPENURMA_ARM64_SYSROOT:-}"

if [[ "$build_mode" == cross ]]; then
    [[ -n "$arm64_sysroot" && -x "$arm64_sysroot/bin/busybox" ]] || {
        echo "[build-all] cross-build requires an ARM64 sysroot with static BusyBox" >&2
        exit 2
    }
    busybox_arm64="$arm64_sysroot/bin/busybox"
else
    busybox_arm64=/bin/busybox
fi

export OPENURMA_LAB_ROOT="$lab"
export OPENURMA_ROOT="$openurma"
export OPENCLICKNP_ROOT="$openclicknp"
export GEM5_ROOT="$lab/gem5"
export KSRC="$kernel_root"
export UMDK_SRC="$openurma/integration/umdk/vendor/umdk"
export UMMU_DEPS="$lab/deps/ummu"
export UMDK_BUILD_DIR="$lab/artifacts/umdk-build"
export ARTIFACT_DIR="$lab/artifacts/kernel"
export JOBS="$jobs"

echo "[build-all] gem5"
"$lab/scripts/build/build_gem5.sh"
echo "[build-all] official UMDK and UDMA provider"
BUILD_STOCK_UDMA=enable "$lab/scripts/build/build_umdk.sh"
echo "[build-all] OLK-6.6 and official kernel drivers"
"$lab/scripts/build/build_olk66.sh"
echo "[build-all] official UBUS/UMMU/UBASE/UDMA modules"
"$lab/official-udma/build_modules.sh"
echo "[build-all] official UDMA initramfs"
KSRC="$kernel_root" ARM_BUILD="$UMDK_BUILD_DIR" BUSYBOX_ARM64="$busybox_arm64" \
    OPENURMA_ARM64_SYSROOT="$arm64_sysroot" \
    OUT="$lab/out/official-udma.cpio.gz" \
    "$lab/official-udma/build_initramfs.sh"
echo "[build-all] interactive initramfs"
KSRC="$kernel_root" ARM_BUILD="$UMDK_BUILD_DIR" BUSYBOX_ARM64="$busybox_arm64" \
    OPENURMA_ARM64_SYSROOT="$arm64_sysroot" \
    OUT="$lab/out/openurma-interactive.cpio.gz" \
    "$lab/scripts/build/build-interactive-initramfs.sh"

for artifact in \
    "$lab/gem5/build/ARM/gem5.opt" \
    "$lab/artifacts/kernel/vmlinux" \
    "$lab/artifacts/umdk-build/urma/tools/urma_perftest/urma_perftest" \
    "$lab/artifacts/umdk-build/urma/hw/udma/liburma-udma.so" \
    "$lab/out/official-udma.cpio.gz" \
    "$lab/out/openurma-interactive.cpio.gz"; do
    [[ -s "$artifact" ]] || { echo "[build-all] missing $artifact" >&2; exit 1; }
done
echo "[build-all] PASS: complete runnable stack is ready"
