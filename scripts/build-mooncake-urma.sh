#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab_dir="$(cd "$script_dir/.." && pwd)"
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
    die "UMDK ARM64 artifacts are missing; run ./setup.sh first"
[[ -f "$lab_dir/artifacts/umdk-build/urma/common/liburma_common.so.0" ]] ||
    die "liburma_common.so.0 is missing; run ./setup.sh first"
[[ "$build_dir_name" != */* ]] || die "MOONCAKE_URMA_BUILD_DIR must be a directory name"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die "JOBS must be a positive integer"
command -v docker >/dev/null || die "docker is required"

actual_mooncake_commit="$(git -C "$mooncake_dir" rev-parse HEAD)"
[[ "$actual_mooncake_commit" == "$expected_mooncake_commit" ]] ||
    die "Mooncake revision is $actual_mooncake_commit; expected $expected_mooncake_commit (set MOONCAKE_REVISION to override deliberately)"

if ! docker image inspect "$docker_image" >/dev/null 2>&1; then
    note "building reproducible ARM64 builder image $docker_image"
    docker build --platform linux/arm64 \
        -f "$lab_dir/docker/mooncake-urma-builder.Dockerfile" \
        -t "$docker_image" "$lab_dir"
fi

container_mooncake=/workspace/Mooncake
container_lab=/workspace/openurma-gem5-lab
container_artifact=/workspace/mooncake-urma-artifact
container_build="$container_mooncake/$build_dir_name"
urma_include="$container_lab/sources/OpenURMA/integration/umdk/vendor/umdk/src/urma/lib/urma/core/include"
urma_library="$container_lab/artifacts/umdk-build/urma/lib/urma/core/liburma.so"
urma_common_dir="$container_lab/artifacts/umdk-build/urma/common"

note "building Mooncake ${actual_mooncake_commit:0:12} with USE_UB=ON"
mkdir -p "$artifact_dir/bin" "$artifact_dir/lib"
docker run --rm --platform linux/arm64 \
    -v "$mooncake_dir:$container_mooncake" \
    -v "$lab_dir:$container_lab" \
    -v "$artifact_dir:$container_artifact" \
    -w "$container_mooncake" \
    "$docker_image" bash -lc "
set -euo pipefail
cmake -S '$container_mooncake' -B '$container_build' -GNinja \\
  -DCMAKE_BUILD_TYPE=Release \\
  -DBUILD_EXAMPLES=ON \\
  -DUSE_UB=ON \\
  -DURMA_SYSTEM_INCLUDE_DIR='$urma_include' \\
  -DURMA_LIBRARY='$urma_library' \\
  -DCMAKE_EXE_LINKER_FLAGS='-Wl,-rpath-link,$urma_common_dir'
cmake --build '$container_build' --target transfer_engine_bench -j'$jobs'
binary='$container_build/mooncake-transfer-engine/example/transfer_engine_bench'
libasio='$container_build/mooncake-common/libasio.so'
readelf -h \"\$binary\" | grep -q 'Machine:.*AArch64'
readelf -d \"\$binary\" | grep -q 'Shared library: \[liburma.so.0\]'
{ LD_LIBRARY_PATH='$container_lab/artifacts/umdk-build/urma/lib/urma/core:$urma_common_dir:'\"\$(dirname \"\$libasio\")\" \\
    \"\$binary\" --help 2>&1 || true; } | grep -q 'protocol'
runtime_path='$container_lab/artifacts/umdk-build/urma/lib/urma/core:$urma_common_dir:'\"\$(dirname \"\$libasio\")\"
for elf in \"\$binary\" \"\$libasio\"; do
  LD_LIBRARY_PATH=\"\$runtime_path\" ldd \"\$elf\"
done | awk '\$2 == \"=>\" && \$3 ~ /^\\// { print \$3 }' | sort -u |
while read -r dep; do
  cp -L \"\$dep\" '$container_artifact/lib/'\"\$(basename \"\$dep\")\"
done
"

install -m 0755 \
    "$mooncake_dir/$build_dir_name/mooncake-transfer-engine/example/transfer_engine_bench" \
    "$artifact_dir/bin/transfer_engine_bench"
install -m 0755 \
    "$mooncake_dir/$build_dir_name/mooncake-common/libasio.so" \
    "$artifact_dir/lib/libasio.so"

sha256_file() {
    if command -v sha256sum >/dev/null; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

manifest="$artifact_dir/manifest.txt"
{
    printf 'mooncake_commit=%s\n' "$(git -C "$mooncake_dir" rev-parse HEAD)"
    printf 'umdk_commit=%s\n' "$(git -C "$lab_dir/sources/OpenURMA/integration/umdk/vendor/umdk" rev-parse HEAD)"
    printf 'use_ub=ON\n'
    printf 'build_type=Release\n'
    printf 'binary=%s\n' "$artifact_dir/bin/transfer_engine_bench"
    printf 'binary_sha256=%s\n' "$(sha256_file "$artifact_dir/bin/transfer_engine_bench")"
    printf 'libasio=%s\n' "$artifact_dir/lib/libasio.so"
    printf 'libasio_sha256=%s\n' "$(sha256_file "$artifact_dir/lib/libasio.so")"
} > "$manifest"

note "wrote $artifact_dir/bin/transfer_engine_bench"
note "manifest: $manifest"
