#!/usr/bin/env bash
# Build helper shared by native Ubuntu and the Docker wrapper.
set -euo pipefail

: "${MOONCAKE_DIR:?MOONCAKE_DIR is required}"
: "${OPENURMA_LAB_ROOT:?OPENURMA_LAB_ROOT is required}"
: "${MOONCAKE_URMA_ARTIFACT_DIR:?MOONCAKE_URMA_ARTIFACT_DIR is required}"

build_dir_name="${MOONCAKE_URMA_BUILD_DIR:-build-urma}"
jobs="${JOBS:-2}"
lab_dir="$OPENURMA_LAB_ROOT"
artifact_dir="$MOONCAKE_URMA_ARTIFACT_DIR"
build_dir="$MOONCAKE_DIR/$build_dir_name"
urma_include="$lab_dir/sources/OpenURMA/integration/umdk/vendor/umdk/src/urma/lib/urma/core/include"
urma_library="$lab_dir/artifacts/umdk-build/urma/lib/urma/core/liburma.so"
urma_common_dir="$lab_dir/artifacts/umdk-build/urma/common"

cmake -S "$MOONCAKE_DIR" -B "$build_dir" -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_EXAMPLES=ON \
    -DUSE_UB=ON \
    -DURMA_SYSTEM_INCLUDE_DIR="$urma_include" \
    -DURMA_LIBRARY="$urma_library" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath-link,$urma_common_dir"
cmake --build "$build_dir" --target transfer_engine_bench -j"$jobs"

binary="$build_dir/mooncake-transfer-engine/example/transfer_engine_bench"
libasio="$build_dir/mooncake-common/libasio.so"
readelf -h "$binary" | grep -q 'Machine:.*AArch64'
readelf -d "$binary" | grep -q 'Shared library: \[liburma.so.0\]'
runtime_path="$lab_dir/artifacts/umdk-build/urma/lib/urma/core:$urma_common_dir:$(dirname "$libasio")"
{ LD_LIBRARY_PATH="$runtime_path" "$binary" --help 2>&1 || true; } |
    grep -q 'protocol'

rm -rf -- "$artifact_dir/bin" "$artifact_dir/lib"
install -d "$artifact_dir/bin" "$artifact_dir/lib"
for elf in "$binary" "$libasio"; do
    LD_LIBRARY_PATH="$runtime_path" ldd "$elf"
done | awk '$2 == "=>" && $3 ~ /^\// { print $3 }' |
    sort -u |
while read -r dep; do
    cp -L "$dep" "$artifact_dir/lib/$(basename "$dep")"
done

strip --strip-unneeded "$binary" "$libasio"
install -m 0755 "$binary" "$artifact_dir/bin/transfer_engine_bench"
install -m 0755 "$libasio" "$artifact_dir/lib/libasio.so"

sha256_file() { sha256sum "$1" | awk '{print $1}'; }
cat > "$artifact_dir/manifest.txt" <<EOF
mooncake_commit=$(git -C "$MOONCAKE_DIR" rev-parse HEAD)
umdk_commit=$(git -C "$lab_dir/sources/OpenURMA/integration/umdk/vendor/umdk" rev-parse HEAD)
use_ub=ON
build_type=Release
binary=bin/transfer_engine_bench
binary_sha256=$(sha256_file "$artifact_dir/bin/transfer_engine_bench")
libasio=lib/libasio.so
libasio_sha256=$(sha256_file "$artifact_dir/lib/libasio.so")
EOF
