#!/usr/bin/env bash
# Reproduce the complete ARM64 lab or x86_64 UMDK target on Ubuntu 22.04.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab_root="$(cd "$script_dir/.." && pwd)"
jobs="${JOBS:-2}"
sources_only=0
install_deps=1
target_arch="${OPENURMA_TARGET_ARCH:-arm64}"

usage() {
    cat <<'EOF'
Usage: ./lab --runtime native setup [--target-arch arm64|x86_64]
       [--sources-only] [--skip-deps] [--jobs N]

Install Ubuntu dependencies, fetch every pinned source revision, and build the
complete lab directly on ARM64 Ubuntu 22.04. The x86_64 target builds the
official UMDK userspace/provider only because the official UB kernel and the
current gem5 platform are ARM64-only. --skip-deps is useful on provisioned hosts.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sources-only) sources_only=1; shift ;;
        --skip-deps) install_deps=0; shift ;;
        --target-arch) [[ $# -ge 2 ]] || { usage >&2; exit 2; }; target_arch=$2; shift 2 ;;
        --target-arch=*) target_arch=${1#*=}; shift ;;
        --jobs) [[ $# -ge 2 ]] || { usage >&2; exit 2; }; jobs=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'setup-native.sh: unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "setup-native.sh: --jobs must be positive" >&2; exit 2; }
[[ "$(uname -s)" == Linux ]] || { echo "setup-native.sh: Linux is required" >&2; exit 2; }
case "$target_arch" in
    arm64|aarch64) target_arch=arm64; required_host_arch=aarch64 ;;
    x86_64|amd64) target_arch=x86_64; required_host_arch=x86_64 ;;
    *) echo "setup-native.sh: --target-arch must be arm64 or x86_64" >&2; exit 2 ;;
esac
[[ "$(uname -m)" == "$required_host_arch" ]] || {
    echo "setup-native.sh: $target_arch requires native $required_host_arch Ubuntu" >&2
    exit 2
}

(( install_deps == 0 )) || "$script_dir/install-ubuntu-deps.sh"

export OPENURMA_EXECUTION_MODE=native
export OPENURMA_LAB_ROOT="${OPENURMA_LAB_ROOT:-$lab_root}"
export KSRC="${KSRC:-$OPENURMA_LAB_ROOT/oe66}"
export JOBS="$jobs"
export OPENURMA_TARGET_ARCH="$target_arch"

echo "[native-setup] fetching pinned source trees"
"$script_dir/fetch-sources.sh"
if (( sources_only )); then
    echo "[native-setup] source preparation passed"
    exit 0
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
