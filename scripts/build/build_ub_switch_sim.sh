#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="${UBSIM_LAB_ROOT:-$(cd "$script_dir/../.." && pwd)}"
build_dir="${UBSIM_UB_SWITCH_BUILD:-$lab/artifacts/ub-switch-sim-build}"
jobs="${JOBS:-2}"

[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "JOBS must be positive" >&2; exit 2; }
cmake -S "$lab/components/ub-switch-sim" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build "$build_dir" --parallel "$jobs"
ctest --test-dir "$build_dir" --output-on-failure
