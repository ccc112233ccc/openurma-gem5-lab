#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab_dir="$(cd "$script_dir/.." && pwd)"
# shellcheck source=runtime.sh
source "$script_dir/runtime.sh"
# This builder is also the native producer for the QEMU adapter stack on
# Apple Silicon.  The general lab runtime remains Linux-or-Docker only; limit
# the exception to this portable host-side component build.
if [[ "$OPENURMA_EXECUTION_MODE" == native && "$(uname -s)" == Darwin ]]; then
    ou_runtime_validate() { :; }
fi
container="$OPENURMA_CONTAINER"
runtime_lab="$(ou_runtime_default_lab "$lab_dir")"
if [[ "$OPENURMA_EXECUTION_MODE" == docker ]]; then
    default_source_root="$runtime_lab/sources/ns-3-ub"
else
    default_source_root="$lab_dir/sources/ns-3-ub"
fi
source_root="${OPENURMA_NS3UB_ROOT:-$default_source_root}"
build_suffix=linux
if [[ "$OPENURMA_EXECUTION_MODE" == native && "$(uname -s)" == Darwin ]]; then
    build_suffix=macos
fi
cache_dir="${OPENURMA_NS3UB_CACHE:-$source_root/cmake-cache-$build_suffix}"
output_dir="${OPENURMA_NS3UB_OUTPUT:-$source_root/build-$build_suffix}"
ubnet_adapter_source="$runtime_lab/integrations/ns3ub/ub-net-adapter.cc"
ubnet_adapter_cmake="$runtime_lab/integrations/ns3ub/ub-net-adapter.CMakeLists.txt"
simbricks_base_source="$runtime_lab/components/udma-device-sim/simbricks_base_portable.c"
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
# Remove files installed by the retired ring-v4 adapter. The source checkout is
# generated and pinned, so restoring this one upstream build file is safe and
# keeps repeated builds independent of the removed compatibility path.
ou_exec git -C "$source_root" restore --source "$expected_source_revision" -- \
    src/unified-bus/CMakeLists.txt
ou_exec rm -f "$source_root/scratch/ub-gem5-adapter.cc" \
    "$source_root/src/unified-bus/model/ub-external-adapter-protocol.h"
ou_exec mkdir -p "$source_root/scratch/openurma-ub-net"
ou_exec install -m 0644 "$ubnet_adapter_source" \
    "$source_root/scratch/openurma-ub-net/ub-net-adapter.cc"
ou_exec install -m 0644 "$ubnet_adapter_cmake" \
    "$source_root/scratch/openurma-ub-net/CMakeLists.txt"
ou_exec install -m 0644 "$simbricks_base_source" \
    "$source_root/scratch/openurma-ub-net/simbricks-base-portable.c"
if ! ou_exec mkdir -p "$cache_dir" "$output_dir/include/ns3" 2>/dev/null; then
    cache_dir=/tmp/ns3ub-modular-cache
    output_dir=/tmp/ns3ub-modular-build
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
    -DOPENURMA_LAB_ROOT="$runtime_lab" \
    -DNS3_OUTPUT_DIRECTORY="$output_dir"

ou_exec cmake --build "$cache_dir" \
    --target scratch_ub-net-adapter \
    -j "${OPENURMA_BUILD_JOBS:-8}"

ou_exec test -x "$output_dir/scratch/ns3.44-ub-net-adapter"

if [[ "$OPENURMA_EXECUTION_MODE" == native && "$(uname -s)" == Darwin ]]; then
    echo "Skipping Linux-only UB switch process contracts on macOS; the native adapter binary was linked and checked."
else
    # Build the protocol peer from the same pinned SimBricks tree and exercise
    # asynchronous and conservative-synchronization contracts end to end.
    switch_build=/tmp/openurma-ub-switch-ns3-build
    ou_exec env OPENURMA_LAB_ROOT="$runtime_lab" \
        OPENURMA_UB_SWITCH_BUILD="$switch_build" \
        JOBS="${OPENURMA_BUILD_JOBS:-8}" \
        bash "$runtime_lab/scripts/build/build_ub_switch_sim.sh"
    for sync_mode in off required; do
        ou_exec timeout 20 bash \
            "$runtime_lab/components/ub-switch-sim/tests/process_contract.sh" \
            "$output_dir/scratch/ns3.44-ub-net-adapter" \
            "$switch_build/ub-net-contract-peer" \
            "/tmp/openurma-ns3ub-contract-$sync_mode" "$sync_mode"
    done
fi
echo "ns-3-UB UB-NET adapter built: $output_dir/scratch/ns3.44-ub-net-adapter"
