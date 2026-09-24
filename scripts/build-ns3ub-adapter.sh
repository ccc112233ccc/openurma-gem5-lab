#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab_dir="$(cd "$script_dir/.." && pwd)"
# shellcheck source=runtime.sh
source "$script_dir/runtime.sh"
container="$OPENURMA_CONTAINER"
runtime_lab="$(ou_runtime_default_lab "$lab_dir")"
if [[ "$OPENURMA_EXECUTION_MODE" == docker ]]; then
    default_source_root="$runtime_lab/sources/ns-3-ub"
else
    default_source_root="$lab_dir/sources/ns-3-ub"
fi
source_root="${OPENURMA_NS3UB_ROOT:-$default_source_root}"
cache_dir="${OPENURMA_NS3UB_CACHE:-$source_root/cmake-cache-linux}"
output_dir="${OPENURMA_NS3UB_OUTPUT:-$source_root/build-linux}"
adapter_source="$runtime_lab/integrations/ns3ub/ub-gem5-adapter.cc"
adapter_protocol="$runtime_lab/integrations/ns3ub/ub-external-adapter-protocol.h"
adapter_cmake_patch="$runtime_lab/integrations/ns3ub/register-adapter-header.patch"
expected_source_revision=d6aa9e242d5a93f5bbd1ad54f39b1620c1b8757b

# Docker Desktop can retain an unreachable virtiofs directory after the host
# switches between source branches that add/remove a build directory. A clean
# workspace uses the source-local paths; a stale mount transparently falls
# back to container-local caches instead of failing during CMake configure.
ou_runtime_start
# The complete adapter is lab-owned and checked in under integrations/. The
# upstream ns-3-UB checkout supplies the fabric model at one pinned revision.
# Install the overlay explicitly so a fresh clone never depends on unpublished
# commits in a sibling repository.
ou_exec test -f "$source_root/CMakeLists.txt" || {
    echo "ns-3-UB source is missing at $source_root; run './lab setup --sources-only'" >&2
    exit 2
}
actual_source_revision="$(ou_exec git -C "$source_root" rev-parse HEAD)"
[[ "$actual_source_revision" == "$expected_source_revision" ]] || {
    echo "ns-3-UB is at $actual_source_revision; expected $expected_source_revision" >&2
    exit 2
}
ou_exec install -m 0644 "$adapter_source" "$source_root/scratch/ub-gem5-adapter.cc"
ou_exec install -m 0644 "$adapter_protocol" \
    "$source_root/src/unified-bus/model/ub-external-adapter-protocol.h"
if ou_exec git -C "$source_root" apply --reverse --check "$adapter_cmake_patch" \
        >/dev/null 2>&1; then
    : # already applied
elif ou_exec git -C "$source_root" apply --check "$adapter_cmake_patch"; then
    ou_exec git -C "$source_root" apply "$adapter_cmake_patch"
else
    echo "ns-3-UB CMake integration does not match the pinned source" >&2
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
ou_exec env PYTHONPATH="$runtime_lab/tools:$runtime_lab" python3 \
    "$runtime_lab/tools/test_ns3ub_native_adapter.py" \
    "$output_dir/scratch/ns3.44-ub-gem5-adapter"
echo "ns-3-UB gem5 adapter built: $output_dir/scratch/ns3.44-ub-gem5-adapter"
