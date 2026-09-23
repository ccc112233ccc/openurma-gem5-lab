#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab_dir="$(cd "$script_dir/.." && pwd)"
: "${OPENURMA_CONTAINER:=openurma-repro-20260909}"
# shellcheck source=runtime.sh
source "$script_dir/runtime.sh"
container="$OPENURMA_CONTAINER"
runtime_lab="$(ou_runtime_default_lab "$lab_dir")"
if [[ "$OPENURMA_EXECUTION_MODE" == docker ]]; then
    default_source_root=/workspace/ns-3-ub
else
    default_source_root="$(dirname "$lab_dir")/ns-3-ub"
fi
source_root="${OPENURMA_NS3UB_ROOT:-$default_source_root}"
cache_dir="${OPENURMA_NS3UB_CACHE:-$source_root/cmake-cache-linux}"
output_dir="${OPENURMA_NS3UB_OUTPUT:-$source_root/build-linux}"
adapter_patch="$runtime_lab/patches/source/ns3ub/0001-Synchronize-external-adapters-for-simulation-lifetim.patch"

# Docker Desktop can retain an unreachable virtiofs directory after the host
# switches between source branches that add/remove a build directory. A clean
# workspace uses the source-local paths; a stale mount transparently falls
# back to container-local caches instead of failing during CMake configure.
ou_runtime_start
# The public ns-3-UB remote is read-only for this workspace. Keep the adapter
# ABI reproducible by applying the reviewed lifetime-sync change locally. A
# reverse check makes this idempotent for a checkout that already contains the
# commit; unrelated working-tree changes are left untouched.
if ou_exec git -C "$source_root" apply --reverse --check "$adapter_patch" \
        >/dev/null 2>&1; then
    : # already applied or committed
elif ou_exec git -C "$source_root" apply --check "$adapter_patch"; then
    ou_exec git -C "$source_root" apply "$adapter_patch"
else
    echo "ns-3-UB adapter sources do not match the pinned patch base" >&2
    exit 1
fi
if ! ou_exec mkdir -p "$cache_dir" "$output_dir/include/ns3" 2>/dev/null; then
    cache_dir=/tmp/ns3ub-native-cache
    output_dir=/tmp/ns3ub-native-build
    ou_exec mkdir -p "$cache_dir" "$output_dir/include/ns3"
    echo "ns-3-UB source-local build directory is unavailable; using $output_dir" >&2
fi

ou_exec cmake -S "$source_root" -B "$cache_dir" \
    -DCMAKE_BUILD_TYPE=release \
    -DNS3_ASSERT=OFF \
    -DNS3_LOG=OFF \
    -DNS3_WARNINGS_AS_ERRORS=OFF \
    -DNS3_NATIVE_OPTIMIZATIONS=OFF \
    -DNS3_EXAMPLES=OFF \
    -DNS3_MPI=OFF \
    -DNS3_MTP=OFF \
    -DNS3_TESTS=OFF \
    -DNS3_ENABLED_MODULES=unified-bus \
    -DNS3_OUTPUT_DIRECTORY="$output_dir"

ou_exec cmake --build "$cache_dir" \
    --target scratch_ub-gem5-adapter -j "${OPENURMA_BUILD_JOBS:-8}"

ou_exec test -x \
    "$output_dir/scratch/ns3.44-ub-gem5-adapter"
echo "ns-3-UB gem5 adapter built: $output_dir/scratch/ns3.44-ub-gem5-adapter"
