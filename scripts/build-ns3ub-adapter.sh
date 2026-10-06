#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab_dir="$(cd "$script_dir/.." && pwd)"
# shellcheck source=runtime.sh
source "$script_dir/runtime.sh"
# This builder is also the native producer for the QEMU adapter stack on
# Apple Silicon.  The general lab runtime remains Linux-or-Docker only; limit
# the exception to this portable host-side component build.
if [[ "$UBSIM_EXECUTION_MODE" == native && "$(uname -s)" == Darwin ]]; then
    ubsim_runtime_validate() { :; }
fi
container="$UBSIM_CONTAINER"
runtime_lab="$(ubsim_runtime_default_lab "$lab_dir")"
if [[ "$UBSIM_EXECUTION_MODE" == docker ]]; then
    default_source_root="$runtime_lab/sources/ns-3-ub"
else
    default_source_root="$lab_dir/sources/ns-3-ub"
fi
source_root="${UBSIM_NS3UB_ROOT:-$default_source_root}"
build_suffix=linux
if [[ "$UBSIM_EXECUTION_MODE" == native && "$(uname -s)" == Darwin ]]; then
    build_suffix=macos
fi
cache_dir="${UBSIM_NS3UB_CACHE:-$source_root/cmake-cache-$build_suffix}"
output_dir="${UBSIM_NS3UB_OUTPUT:-$source_root/build-$build_suffix}"
ubnet_adapter_source="$runtime_lab/integrations/ns3ub/ub-net-adapter.cc"
ubnet_adapter_cmake="$runtime_lab/integrations/ns3ub/ub-net-adapter.CMakeLists.txt"
ctp_retransmission_patch="$runtime_lab/integrations/ns3ub/patches/0001-ctp-timeout-retransmission.patch"
simbricks_base_source="$runtime_lab/components/udma-device-sim/simbricks_base_portable.c"
expected_source_revision=d6aa9e242d5a93f5bbd1ad54f39b1620c1b8757b

# Docker Desktop can retain an unreachable virtiofs directory after the host
# switches between source branches that add/remove a build directory. A clean
# workspace uses the source-local paths; a stale mount transparently falls
# back to container-local caches instead of failing during CMake configure.
ubsim_runtime_start
# The complete adapter is lab-owned and checked in under integrations/. The
# upstream ns-3-UB checkout supplies the fabric model at one pinned revision.
# Install the overlay explicitly so a fresh clone never depends on unpublished
# commits in a sibling repository.
ubsim_exec test -f "$source_root/CMakeLists.txt" || {
    echo "ns-3-UB source is missing at $source_root; run './lab setup --sources-only'" >&2
    exit 2
}
actual_source_revision="$(ubsim_exec git -C "$source_root" rev-parse HEAD)"
[[ "$actual_source_revision" == "$expected_source_revision" ]] || {
    echo "ns-3-UB is at $actual_source_revision; expected $expected_source_revision" >&2
    exit 2
}
# Remove files installed by the retired ring-v4 adapter. The source checkout is
# generated and pinned, so restoring this one upstream build file is safe and
# keeps repeated builds independent of the removed compatibility path.
ubsim_exec git -C "$source_root" restore --source "$expected_source_revision" -- \
    src/unified-bus/CMakeLists.txt \
    src/unified-bus/model/protocol/ub-ctp.cc \
    src/unified-bus/model/protocol/ub-ctp.h
ubsim_exec git -C "$source_root" apply --check "$ctp_retransmission_patch"
ubsim_exec git -C "$source_root" apply "$ctp_retransmission_patch"
ubsim_exec rm -f "$source_root/scratch/ub-gem5-adapter.cc" \
    "$source_root/src/unified-bus/model/ub-external-adapter-protocol.h"
ubsim_exec mkdir -p "$source_root/scratch/ubsim-ub-net"
ubsim_exec install -m 0644 "$ubnet_adapter_source" \
    "$source_root/scratch/ubsim-ub-net/ub-net-adapter.cc"
ubsim_exec install -m 0644 "$ubnet_adapter_cmake" \
    "$source_root/scratch/ubsim-ub-net/CMakeLists.txt"
ubsim_exec install -m 0644 "$simbricks_base_source" \
    "$source_root/scratch/ubsim-ub-net/simbricks-base-portable.c"
if ! ubsim_exec mkdir -p "$cache_dir" "$output_dir/include/ns3" 2>/dev/null; then
    cache_dir=/tmp/ns3ub-modular-cache
    output_dir=/tmp/ns3ub-modular-build
    ubsim_exec mkdir -p "$cache_dir" "$output_dir/include/ns3"
    echo "ns-3-UB source-local build directory is unavailable; using $output_dir" >&2
fi

ubsim_exec cmake -S "$source_root" -B "$cache_dir" \
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
    -DUBSIM_LAB_ROOT="$runtime_lab" \
    -DNS3_OUTPUT_DIRECTORY="$output_dir"

ubsim_exec cmake --build "$cache_dir" \
    --target scratch_ub-net-adapter \
    -j "${UBSIM_BUILD_JOBS:-8}"

ubsim_exec test -x "$output_dir/scratch/ns3.44-ub-net-adapter"

if [[ "$UBSIM_EXECUTION_MODE" == native && "$(uname -s)" == Darwin ]]; then
    # The UB-NET contract peer is portable too.  Reuse the checked-in Darwin
    # build when available so the native CTP order path is exercised on macOS.
    peer="$lab_dir/artifacts/ub-switch-sim-build/ub-net-contract-peer"
    if [[ -x "$peer" ]]; then
        bash "$lab_dir/integrations/ns3ub/tests/order_contract.sh" \
            "$output_dir/scratch/ns3.44-ub-net-adapter" "$peer" \
            /tmp/ubsim-ns3ub-order-contract
        bash "$lab_dir/integrations/ns3ub/tests/retransmission_contract.sh" \
            "$output_dir/scratch/ns3.44-ub-net-adapter" "$peer" \
            /tmp/ubsim-ns3ub-retrans-contract
    else
        echo "Skipping process contracts on macOS; build ub-switch-sim first to enable them."
    fi
else
    # Build the protocol peer from the same pinned SimBricks tree and exercise
    # asynchronous and conservative-synchronization contracts end to end.
    switch_build=/tmp/ubsim-ub-switch-ns3-build
    ubsim_exec env UBSIM_LAB_ROOT="$runtime_lab" \
        UBSIM_UB_SWITCH_BUILD="$switch_build" \
        JOBS="${UBSIM_BUILD_JOBS:-8}" \
        bash "$runtime_lab/scripts/build/build_ub_switch_sim.sh"
    for sync_mode in off required; do
        ubsim_exec timeout 20 bash \
            "$runtime_lab/components/ub-switch-sim/tests/process_contract.sh" \
            "$output_dir/scratch/ns3.44-ub-net-adapter" \
            "$switch_build/ub-net-contract-peer" \
            "/tmp/ubsim-ns3ub-contract-$sync_mode" "$sync_mode"
    done
    ubsim_exec timeout 20 bash \
        "$runtime_lab/integrations/ns3ub/tests/order_contract.sh" \
        "$output_dir/scratch/ns3.44-ub-net-adapter" \
        "$switch_build/ub-net-contract-peer" \
        /tmp/ubsim-ns3ub-order-contract
    ubsim_exec timeout 20 bash \
        "$runtime_lab/integrations/ns3ub/tests/retransmission_contract.sh" \
        "$output_dir/scratch/ns3.44-ub-net-adapter" \
        "$switch_build/ub-net-contract-peer" \
        /tmp/ubsim-ns3ub-retrans-contract
fi
echo "ns-3-UB UB-NET adapter built: $output_dir/scratch/ns3.44-ub-net-adapter"
