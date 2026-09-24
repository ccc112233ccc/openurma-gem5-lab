#!/usr/bin/env bash
# Reproduce the complete lab directly on an ARM64 Ubuntu 22.04 host.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab_root="$(cd "$script_dir/.." && pwd)"
jobs="${JOBS:-2}"
sources_only=0
install_deps=1

usage() {
    cat <<'EOF'
Usage: ./lab --runtime native setup [--sources-only] [--skip-deps] [--jobs N]

Install Ubuntu dependencies, fetch every pinned source revision, and build the
complete lab directly on an ARM64 Ubuntu 22.04 host. --skip-deps is useful for
an already-provisioned machine or for the Docker wrapper.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sources-only) sources_only=1; shift ;;
        --skip-deps) install_deps=0; shift ;;
        --jobs) [[ $# -ge 2 ]] || { usage >&2; exit 2; }; jobs=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'setup-native.sh: unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "setup-native.sh: --jobs must be positive" >&2; exit 2; }
[[ "$(uname -s)" == Linux ]] || { echo "setup-native.sh: Linux is required" >&2; exit 2; }
[[ "$(uname -m)" == aarch64 ]] || {
    echo "setup-native.sh: native mode is currently validated only on ARM64 Ubuntu" >&2
    exit 2
}

(( install_deps == 0 )) || "$script_dir/install-ubuntu-deps.sh"

export OPENURMA_EXECUTION_MODE=native
export OPENURMA_LAB_ROOT="${OPENURMA_LAB_ROOT:-$lab_root}"
export KSRC="${KSRC:-$OPENURMA_LAB_ROOT/oe66}"
export JOBS="$jobs"

echo "[native-setup] fetching pinned source trees"
"$script_dir/fetch-sources.sh"
if (( sources_only )); then
    echo "[native-setup] source preparation passed"
    exit 0
fi

echo "[native-setup] building the complete stack"
"$script_dir/build-all.sh"
echo "[native-setup] PASS"
echo "Start two nodes with: ./lab --runtime native start --profile fast --provider official"
