#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="${OPENURMA_LAB_ROOT:-$(cd "$script_dir/../.." && pwd)}"
runtime_tag="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
build_dir="${OPENURMA_UDMA_DEVICE_BUILD:-$lab/artifacts/udma-device-sim-build-$runtime_tag}"
jobs="${JOBS:-2}"

[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "JOBS must be positive" >&2; exit 2; }
cmake -S "$lab/components/udma-device-sim" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build "$build_dir" --parallel "$jobs"
ctest --test-dir "$build_dir" --output-on-failure
