#!/usr/bin/env bash
# Build the pinned openEuler UMDK userspace for the ARM64 gem5 guest.
#
# Run this script inside the ARM64 Linux build container.  The build tree is
# deliberately outside the UMDK source tree, so the pinned integration revision
# remains clean and the result can be copied directly into the guest initramfs.
set -euo pipefail

PINNED_UMDK_SHA="097c3a5d6b2234d6a070e3bfb26f6f49f0ce26f8"
UMDK_SRC="${UMDK_SRC:-/workspace/OpenURMA/integration/umdk/vendor/umdk}"
BUILD_DIR="${UMDK_BUILD_DIR:-/workspace/openurma-gem5-lab/artifacts/umdk-build}"
JOBS="${JOBS:-2}"
BUILD_STOCK_UDMA="${BUILD_STOCK_UDMA:-disable}"
ALLOW_DIRTY_UMDK="${ALLOW_DIRTY_UMDK:-disable}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
UMMU_DEPS="${UMMU_DEPS:-$SCRIPT_DIR/deps/ummu}"
UMDK_INTEGRATION_DIR="${UMDK_INTEGRATION_DIR:-$(dirname -- "$(dirname -- "$UMDK_SRC")")}"
UMMU_SHIM_SRC="${UMMU_SHIM_SRC:-$UMDK_INTEGRATION_DIR/ummu_shim}"
UMMU_SHIM_BUILD_DIR="${UMMU_SHIM_BUILD_DIR:-${BUILD_DIR}-ummu-shim}"
GEM5_ROOT="${GEM5_ROOT:-$SCRIPT_DIR/gem5}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
GEM5_M5_LIB="${GEM5_M5_LIB:-$GEM5_ROOT/util/m5/build/arm64/out/libm5.a}"
M5OPS_DISPATCH_DIR="${M5OPS_DISPATCH_DIR:-$SCRIPT_DIR/tools}"

die() {
    echo "ERROR: $*" >&2
    exit 1
}

[[ "$(uname -s)" == "Linux" ]] || die "run this build inside the ARM64 Linux container"
[[ "$(uname -m)" == "aarch64" ]] || die "expected an aarch64 builder, got $(uname -m)"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "JOBS must be a positive integer"
[[ "$BUILD_STOCK_UDMA" == "enable" || "$BUILD_STOCK_UDMA" == "disable" ]] || \
    die "BUILD_STOCK_UDMA must be 'enable' or 'disable'"
[[ "$ALLOW_DIRTY_UMDK" == "enable" || "$ALLOW_DIRTY_UMDK" == "disable" ]] || \
    die "ALLOW_DIRTY_UMDK must be 'enable' or 'disable'"
[[ -n "$BUILD_DIR" && "$BUILD_DIR" != "/" ]] || die "unsafe UMDK_BUILD_DIR: $BUILD_DIR"
[[ -d "$UMDK_SRC/.git" || -f "$UMDK_SRC/.git" ]] || die "UMDK source not found at $UMDK_SRC"
[[ -f "$GEM5_ROOT/include/gem5/m5ops.h" ]] || die "gem5 headers not found at $GEM5_ROOT"
[[ -f "$GEM5_ROOT/util/m5/src/m5_mmap.h" ]] || die "gem5 m5 mmap header is missing"
[[ -f "$M5OPS_DISPATCH_DIR/ou-m5ops.h" ]] || die "OpenURMA m5ops dispatcher is missing"
command -v scons >/dev/null || die "scons is required to build libm5"
command -v "${CROSS_COMPILE}gcc" >/dev/null || \
    die "cross compiler not found: ${CROSS_COMPILE}gcc"

actual_sha="$(git -C "$UMDK_SRC" rev-parse HEAD)"
[[ "$actual_sha" == "$PINNED_UMDK_SHA" ]] || \
    die "UMDK is at $actual_sha, expected $PINNED_UMDK_SHA"

# Do not silently package binaries from a locally edited UMDK checkout.  A
# caller may explicitly allow unrelated experiments, but never changes in the
# stock provider subtree that this script claims to build unchanged.
if [[ "$ALLOW_DIRTY_UMDK" == "enable" ]]; then
    udma_status="$(git -C "$UMDK_SRC" status --porcelain=v1 --untracked-files=all -- \
        src/urma/hw/udma)"
    [[ -z "$udma_status" ]] || {
        printf '%s\n' "$udma_status" >&2
        die "stock UDMA provider subtree differs from pinned commit"
    }
    echo "ALLOW_DIRTY_UMDK=enable: unrelated UMDK changes allowed; stock UDMA subtree is clean"
else
    git -C "$UMDK_SRC" diff --quiet -- || die "UMDK worktree has unstaged source changes"
    git -C "$UMDK_SRC" diff --cached --quiet -- || die "UMDK worktree has staged source changes"
fi

echo "Building UMDK $PINNED_UMDK_SHA for aarch64 with JOBS=$JOBS, stock UDMA=$BUILD_STOCK_UDMA"
rm -rf -- "$BUILD_DIR"

echo "Building gem5 address/instruction pseudo-op library"
scons -C "$GEM5_ROOT/util/m5" \
    "arm64.CROSS_COMPILE=$CROSS_COMPILE" build/arm64/out/libm5.a >/dev/null
[[ -f "$GEM5_M5_LIB" ]] || die "gem5 m5 library was not produced: $GEM5_M5_LIB"

if [[ "$BUILD_STOCK_UDMA" == "enable" ]]; then
    [[ -f "$UMMU_DEPS/include/ummu_api.h" ]] || \
        die "official ummu_api.h not found under $UMMU_DEPS/include"
    [[ -f "$UMMU_DEPS/kernel_headers/ummu_core.h" ]] || \
        die "official ummu_core.h not found under $UMMU_DEPS/kernel_headers"
    [[ -f "$UMMU_SHIM_SRC/CMakeLists.txt" ]] || \
        die "simulation UMMU shim not found at $UMMU_SHIM_SRC"
    [[ -n "$UMMU_SHIM_BUILD_DIR" && "$UMMU_SHIM_BUILD_DIR" != "/" ]] || \
        die "unsafe UMMU_SHIM_BUILD_DIR: $UMMU_SHIM_BUILD_DIR"

    echo "Building simulation-only libummu.so.1 ABI shim"
    rm -rf -- "$UMMU_SHIM_BUILD_DIR"
    cmake -S "$UMMU_SHIM_SRC" -B "$UMMU_SHIM_BUILD_DIR" \
        -DUMMU_API_INCLUDE_DIR="$UMMU_DEPS/include" \
        -DUMMU_UAPI_INCLUDE_DIR="$UMMU_DEPS/kernel_headers" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build "$UMMU_SHIM_BUILD_DIR" --parallel "$JOBS"
    ctest --test-dir "$UMMU_SHIM_BUILD_DIR" --output-on-failure

    # The pinned UMDK build uses plain <ummu_api.h> and -lummu.  Supplying
    # compiler/linker search paths keeps the vendored source unmodified.
    export CPATH="$UMMU_DEPS/include:$UMMU_DEPS/kernel_headers${CPATH:+:$CPATH}"
    export LIBRARY_PATH="$UMMU_SHIM_BUILD_DIR${LIBRARY_PATH:+:$LIBRARY_PATH}"
fi

cmake -S "$UMDK_SRC/src" -B "$BUILD_DIR" \
    -DBUILD_ALL=disable \
    -DBUILD_URMA=enable \
    -DBUILD_UDMA="$BUILD_STOCK_UDMA" \
    -DOPENURMA_GEM5_M5_LIBRARY="$GEM5_M5_LIB" \
    -DOPENURMA_GEM5_M5_INCLUDE_DIR="$GEM5_ROOT/include" \
    -DOPENURMA_GEM5_M5_MMAP_INCLUDE_DIR="$GEM5_ROOT/util/m5/src" \
    -DOPENURMA_GEM5_M5_DISPATCH_INCLUDE_DIR="$M5OPS_DISPATCH_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$BUILD_DIR" --parallel "$JOBS"

liburma="$BUILD_DIR/urma/lib/urma/core/liburma.so.0.0.1"
liburma_common="$BUILD_DIR/urma/common/liburma_common.so.0.0.1"
urma_admin="$BUILD_DIR/urma/tools/urma_admin/urma_admin"
urma_perftest="$BUILD_DIR/urma/tools/urma_perftest/urma_perftest"
stock_udma="$BUILD_DIR/urma/hw/udma/liburma-udma.so"
ummu_shim="$UMMU_SHIM_BUILD_DIR/libummu.so.1"

verify_aarch64() {
    local artifact="$1"
    [[ -f "$artifact" ]] || die "missing artifact: $artifact"
    readelf -h "$artifact" | grep -Eq 'Machine:[[:space:]]+AArch64' || {
        file "$artifact" >&2 || true
        die "artifact is not AArch64 ELF: $artifact"
    }
    printf 'AArch64  %s\n' "$artifact"
}

echo
echo "Verified guest artifacts:"
verify_aarch64 "$liburma"
verify_aarch64 "$liburma_common"
verify_aarch64 "$urma_admin"
verify_aarch64 "$urma_perftest"

if [[ "$BUILD_STOCK_UDMA" == "enable" ]]; then
    verify_aarch64 "$stock_udma"
    verify_aarch64 "$ummu_shim"
    readelf -d "$stock_udma" | grep -Fq 'Shared library: [libummu.so.1]' || \
        die "stock UDMA provider is not linked against libummu.so.1"
    readelf -d "$ummu_shim" | grep -Fq 'Library soname: [libummu.so.1]' || \
        die "simulation UMMU shim has the wrong SONAME"
fi

printf '%s\n' "$PINNED_UMDK_SHA" > "$BUILD_DIR/UMDK_SOURCE_COMMIT"
echo "UMDK build ready at $BUILD_DIR"
if [[ "$BUILD_STOCK_UDMA" == "enable" ]]; then
    echo "Stock UDMA provider ready at $stock_udma"
    echo "Simulation UMMU shim ready at $ummu_shim"
fi
