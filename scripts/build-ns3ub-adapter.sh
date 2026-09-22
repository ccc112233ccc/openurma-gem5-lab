#!/usr/bin/env bash
set -euo pipefail

container="${OPENURMA_CONTAINER:-openurma-repro-20260909}"
source_root="${OPENURMA_NS3UB_ROOT:-/workspace/ns-3-ub}"
cache_dir="${OPENURMA_NS3UB_CACHE:-$source_root/cmake-cache-linux}"
output_dir="${OPENURMA_NS3UB_OUTPUT:-$source_root/build-linux}"

# Docker Desktop can retain an unreachable virtiofs directory after the host
# switches between source branches that add/remove a build directory. A clean
# workspace uses the source-local paths; a stale mount transparently falls
# back to container-local caches instead of failing during CMake configure.
if ! docker exec "$container" mkdir -p "$cache_dir" "$output_dir/include/ns3" 2>/dev/null; then
    cache_dir=/tmp/ns3ub-native-cache
    output_dir=/tmp/ns3ub-native-build
    docker exec "$container" mkdir -p "$cache_dir" "$output_dir/include/ns3"
    echo "ns-3-UB source-local build directory is unavailable; using $output_dir" >&2
fi

docker exec "$container" cmake -S "$source_root" -B "$cache_dir" \
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

docker exec "$container" cmake --build "$cache_dir" \
    --target scratch_ub-gem5-adapter -j "${OPENURMA_BUILD_JOBS:-8}"

docker exec "$container" test -x \
    "$output_dir/scratch/ns3.44-ub-gem5-adapter"
echo "ns-3-UB gem5 adapter built: $output_dir/scratch/ns3.44-ub-gem5-adapter"
