#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab_dir="$(cd "$script_dir/.." && pwd)"
# shellcheck source=runtime.sh
source "$script_dir/runtime.sh"
mooncake_dir="${MOONCAKE_DIR:-$(cd "$lab_dir/../Mooncake" 2>/dev/null && pwd || true)}"
docker_image="${MOONCAKE_URMA_BUILD_IMAGE:-openurma-mooncake-builder:ubuntu22.04-arm64}"
build_dir_name="${MOONCAKE_URMA_BUILD_DIR:-build-urma}"
artifact_dir="${MOONCAKE_URMA_ARTIFACT_DIR:-$lab_dir/artifacts/mooncake-urma}"
jobs="${JOBS:-2}"
expected_mooncake_commit="${MOONCAKE_REVISION:-1a0c0a44214ff61a8a4b2e9d90dfb023dd4703ed}"

die() { printf 'error: %s\n' "$*" >&2; exit 2; }
note() { printf '[mooncake-urma] %s\n' "$*"; }

[[ -n "$mooncake_dir" && -f "$mooncake_dir/CMakeLists.txt" ]] ||
    die "Mooncake checkout not found; clone it next to the lab or set MOONCAKE_DIR"
[[ -f "$lab_dir/artifacts/umdk-build/urma/lib/urma/core/liburma.so" ]] ||
    die "UMDK ARM64 artifacts are missing; run './lab setup' first"
[[ -f "$lab_dir/artifacts/umdk-build/urma/common/liburma_common.so.0" ]] ||
    die "liburma_common.so.0 is missing; run './lab setup' first"
[[ "$build_dir_name" != */* ]] || die "MOONCAKE_URMA_BUILD_DIR must be a directory name"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die "JOBS must be a positive integer"
actual_mooncake_commit="$(git -C "$mooncake_dir" rev-parse HEAD)"
[[ "$actual_mooncake_commit" == "$expected_mooncake_commit" ]] ||
    die "Mooncake revision is $actual_mooncake_commit; expected $expected_mooncake_commit (set MOONCAKE_REVISION to override deliberately)"

note "building Mooncake ${actual_mooncake_commit:0:12} with USE_UB=ON"
if [[ "$OPENURMA_EXECUTION_MODE" == native ]]; then
    [[ "$(uname -s)" == Linux && "$(uname -m)" == aarch64 ]] ||
        die "native Mooncake build requires ARM64 Linux"
    MOONCAKE_DIR="$mooncake_dir" \
    OPENURMA_LAB_ROOT="$lab_dir" \
    MOONCAKE_URMA_BUILD_DIR="$build_dir_name" \
    MOONCAKE_URMA_ARTIFACT_DIR="$artifact_dir" \
    JOBS="$jobs" \
        "$script_dir/build-mooncake-urma-inner.sh"
else
    command -v docker >/dev/null || die "docker is required"
    if ! docker image inspect "$docker_image" >/dev/null 2>&1; then
        note "building reproducible ARM64 builder image $docker_image"
        docker build --platform linux/arm64 \
            -f "$lab_dir/docker/mooncake-urma-builder.Dockerfile" \
            -t "$docker_image" "$lab_dir"
    fi
    mkdir -p "$artifact_dir"
    docker run --rm --platform linux/arm64 \
        -e MOONCAKE_DIR=/workspace/Mooncake \
        -e OPENURMA_LAB_ROOT=/workspace/openurma-gem5-lab \
        -e MOONCAKE_URMA_BUILD_DIR="$build_dir_name" \
        -e MOONCAKE_URMA_ARTIFACT_DIR=/workspace/mooncake-urma-artifact \
        -e JOBS="$jobs" \
        -v "$mooncake_dir:/workspace/Mooncake" \
        -v "$lab_dir:/workspace/openurma-gem5-lab" \
        -v "$artifact_dir:/workspace/mooncake-urma-artifact" \
        -w /workspace/Mooncake \
        "$docker_image" \
        bash /workspace/openurma-gem5-lab/scripts/build-mooncake-urma-inner.sh
fi

note "wrote $artifact_dir/bin/transfer_engine_bench"
note "manifest: $artifact_dir/manifest.txt"
