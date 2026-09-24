#!/usr/bin/env bash
# Reproduce the complete ARM64 lab or x86_64 UMDK target on Ubuntu 22.04.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab_root="$(cd "$script_dir/.." && pwd)"
jobs="${JOBS:-2}"
sources_only=0
install_deps=1
target_arch="${OPENURMA_TARGET_ARCH:-arm64}"
build_mode="${OPENURMA_BUILD_MODE:-auto}"
arm64_sysroot="${OPENURMA_ARM64_SYSROOT:-}"

usage() {
    cat <<'EOF'
Usage: ./lab --runtime native setup [--target-arch arm64|x86_64]
       [--build-mode auto|native|cross] [--arm64-sysroot PATH]
       [--sources-only] [--skip-deps] [--jobs N]

Install Ubuntu dependencies, fetch every pinned source revision, and build the
complete lab on Ubuntu 22.04. An x86_64 host cross-builds the ARM64 kernel,
drivers, UMDK, initramfs and ARM gem5 guest without an ARM64 container. The
x86_64 target builds UMDK userspace/provider only. --skip-deps is useful on
provisioned hosts.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sources-only) sources_only=1; shift ;;
        --skip-deps) install_deps=0; shift ;;
        --target-arch) [[ $# -ge 2 ]] || { usage >&2; exit 2; }; target_arch=$2; shift 2 ;;
        --target-arch=*) target_arch=${1#*=}; shift ;;
        --build-mode) [[ $# -ge 2 ]] || { usage >&2; exit 2; }; build_mode=$2; shift 2 ;;
        --build-mode=*) build_mode=${1#*=}; shift ;;
        --arm64-sysroot) [[ $# -ge 2 ]] || { usage >&2; exit 2; }; arm64_sysroot=$2; shift 2 ;;
        --arm64-sysroot=*) arm64_sysroot=${1#*=}; shift ;;
        --jobs) [[ $# -ge 2 ]] || { usage >&2; exit 2; }; jobs=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'setup-native.sh: unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "setup-native.sh: --jobs must be positive" >&2; exit 2; }
[[ "$(uname -s)" == Linux ]] || { echo "setup-native.sh: Linux is required" >&2; exit 2; }
case "$target_arch" in
    arm64|aarch64) target_arch=arm64 ;;
    x86_64|amd64) target_arch=x86_64 ;;
    *) echo "setup-native.sh: --target-arch must be arm64 or x86_64" >&2; exit 2 ;;
esac
host_arch="$(uname -m)"
case "$build_mode" in
    auto)
        if [[ "$target_arch" == arm64 && "$host_arch" == x86_64 ]]; then build_mode=cross; else build_mode=native; fi
        ;;
    native|cross) ;;
    *) echo "setup-native.sh: --build-mode must be auto, native, or cross" >&2; exit 2 ;;
esac
if [[ "$target_arch" == arm64 && "$build_mode" == native && "$host_arch" != aarch64 ]]; then
    echo "setup-native.sh: native ARM64 build requires aarch64 Ubuntu" >&2; exit 2
fi
if [[ "$target_arch" == arm64 && "$build_mode" == cross && "$host_arch" != x86_64 ]]; then
    echo "setup-native.sh: ARM64 cross-build requires x86_64 Ubuntu" >&2; exit 2
fi
if [[ "$target_arch" == x86_64 && ( "$build_mode" != native || "$host_arch" != x86_64 ) ]]; then
    echo "setup-native.sh: x86_64 target requires native x86_64 Ubuntu" >&2; exit 2
fi

(( install_deps == 0 )) || "$script_dir/install-ubuntu-deps.sh"

export OPENURMA_EXECUTION_MODE=native
export OPENURMA_LAB_ROOT="${OPENURMA_LAB_ROOT:-$lab_root}"
export KSRC="${KSRC:-$OPENURMA_LAB_ROOT/oe66}"
export JOBS="$jobs"
export OPENURMA_TARGET_ARCH="$target_arch"
export OPENURMA_BUILD_MODE="$build_mode"

echo "[native-setup] fetching pinned source trees"
"$script_dir/fetch-sources.sh"
if (( sources_only )); then
    echo "[native-setup] source preparation passed"
    exit 0
fi

if [[ "$target_arch" == arm64 && "$build_mode" == cross ]]; then
    if [[ -z "$arm64_sysroot" ]]; then
        arm64_sysroot="$OPENURMA_LAB_ROOT/artifacts/sysroots/ubuntu-22.04-arm64"
        export OPENURMA_ARM64_SYSROOT="$arm64_sysroot"
        "$script_dir/build/prepare-arm64-sysroot.sh"
    else
        export OPENURMA_ARM64_SYSROOT="$arm64_sysroot"
    fi
    [[ -x "$OPENURMA_ARM64_SYSROOT/bin/busybox" ]] || {
        echo "setup-native.sh: ARM64 sysroot lacks /bin/busybox: $OPENURMA_ARM64_SYSROOT" >&2
        exit 2
    }
    echo "[native-setup] direct x86_64 -> ARM64 cross-build using $OPENURMA_ARM64_SYSROOT"
fi

if [[ "$target_arch" == x86_64 ]]; then
    echo "[native-setup] building x86_64 UMDK userspace and stock UDMA provider"
    BUILD_STOCK_UDMA=enable "$script_dir/build/build_umdk.sh" --target-arch x86_64
    echo "[native-setup] PASS: x86_64 userspace target"
    echo "Full-system kernel/initramfs launch remains ARM64-only."
    exit 0
fi

echo "[native-setup] building the complete ARM64 stack"
"$script_dir/build-all.sh"
echo "[native-setup] PASS"
echo "Start two nodes with: ./lab --runtime native start --profile fast --provider official"
