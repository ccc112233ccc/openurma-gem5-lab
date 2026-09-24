#!/usr/bin/env bash
# Build the pinned openEuler UMDK userspace for an ARM64 or x86_64 target.
#
# Run this on Linux, either natively or inside the build container. The build tree is
# deliberately outside the UMDK source tree, so the pinned integration revision
# remains clean and the result can be copied directly into the guest initramfs.
set -euo pipefail

PINNED_UMDK_SHA="f84b90b8ddd8173b851334f55d332783d248bfc7"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
LAB_DIR="${OPENURMA_LAB_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
UMDK_SRC="${UMDK_SRC:-$LAB_DIR/sources/OpenURMA/integration/umdk/vendor/umdk}"
TARGET_ARCH="${OPENURMA_TARGET_ARCH:-arm64}"
BUILD_MODE="${OPENURMA_BUILD_MODE:-auto}"
ARM64_SYSROOT="${OPENURMA_ARM64_SYSROOT:-}"
JOBS="${JOBS:-2}"
BUILD_STOCK_UDMA="${BUILD_STOCK_UDMA:-disable}"
ALLOW_DIRTY_UMDK="${ALLOW_DIRTY_UMDK:-disable}"
UMMU_DEPS="${UMMU_DEPS:-$LAB_DIR/deps/ummu}"
UMDK_INTEGRATION_DIR="${UMDK_INTEGRATION_DIR:-$(dirname -- "$(dirname -- "$UMDK_SRC")")}"
GEM5_ROOT="${GEM5_ROOT:-$LAB_DIR/gem5}"
M5OPS_DISPATCH_DIR="${M5OPS_DISPATCH_DIR:-$LAB_DIR/tools}"

die() {
    echo "ERROR: $*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: ./lab build umdk [--target-arch arm64|x86_64]
       [--build-mode auto|native|cross] [--arm64-sysroot PATH]

Build the unmodified UMDK userspace stack for the selected Linux ABI. ARM64 is
the complete full-system target. x86_64 builds UMDK and its provider on native
x86_64 Linux; the official OLK UB/UMMU kernel stack and gem5 machine remain
ARM64-only. On x86_64 Linux, ARM64 can be cross-built directly with an ARM64
sysroot; no ARM64 container is required.
EOF
}

while (( $# > 0 )); do
    case "$1" in
        --target-arch)
            (( $# >= 2 )) || die "--target-arch requires a value"
            TARGET_ARCH=$2
            shift 2
            ;;
        --target-arch=*)
            TARGET_ARCH=${1#*=}
            shift
            ;;
        --build-mode)
            (( $# >= 2 )) || die "--build-mode requires a value"
            BUILD_MODE=$2
            shift 2
            ;;
        --build-mode=*) BUILD_MODE=${1#*=}; shift ;;
        --arm64-sysroot)
            (( $# >= 2 )) || die "--arm64-sysroot requires a value"
            ARM64_SYSROOT=$2
            shift 2
            ;;
        --arm64-sysroot=*) ARM64_SYSROOT=${1#*=}; shift ;;
        -h|--help)
            usage
            exit 0
            ;;
        *) die "unknown option: $1" ;;
    esac
done

case "$TARGET_ARCH" in
    arm64|aarch64)
        TARGET_ARCH=arm64
        TARGET_MACHINE_RE='Machine:[[:space:]]+AArch64'
        TARGET_MACHINE_NAME=AArch64
        M5_ABI=arm64
        default_build_dir="$LAB_DIR/artifacts/umdk-build"
        default_cross_compile=aarch64-linux-gnu-
        ;;
    x86_64|amd64)
        TARGET_ARCH=x86_64
        TARGET_MACHINE_RE='Machine:[[:space:]]+(Advanced Micro Devices X86-64|AMD x86-64)'
        TARGET_MACHINE_NAME=x86_64
        M5_ABI=x86
        default_build_dir="$LAB_DIR/artifacts/umdk-build-x86_64"
        default_cross_compile=
        ;;
    *) die "target architecture must be arm64 or x86_64" ;;
esac

BUILD_DIR="${UMDK_BUILD_DIR:-$default_build_dir}"
UMMU_SHIM_SRC="${UMMU_SHIM_SRC:-$UMDK_INTEGRATION_DIR/ummu_shim}"
UMMU_SHIM_BUILD_DIR="${UMMU_SHIM_BUILD_DIR:-${BUILD_DIR}-ummu-shim}"
CROSS_COMPILE="${CROSS_COMPILE-$default_cross_compile}"
GEM5_M5_LIB="${GEM5_M5_LIB:-$GEM5_ROOT/util/m5/build/$M5_ABI/out/libm5.a}"

[[ "$(uname -s)" == "Linux" ]] || die "UMDK target builds require Linux"
host_arch="$(uname -m)"
case "$BUILD_MODE" in
    auto)
        if [[ "$TARGET_ARCH" == arm64 && "$host_arch" == x86_64 ]]; then
            BUILD_MODE=cross
        else
            BUILD_MODE=native
        fi
        ;;
    native|cross) ;;
    *) die "build mode must be auto, native, or cross" ;;
esac
if [[ "$TARGET_ARCH" == x86_64 ]]; then
    [[ "$host_arch" == x86_64 && "$BUILD_MODE" == native ]] || \
        die "x86_64 UMDK currently requires a native x86_64 Linux builder"
elif [[ "$BUILD_MODE" == native ]]; then
    [[ "$host_arch" == aarch64 ]] || die "native ARM64 UMDK requires an aarch64 Linux builder"
else
    [[ "$host_arch" == x86_64 ]] || die "ARM64 cross-build is supported from x86_64 Linux"
    [[ -n "$ARM64_SYSROOT" && -d "$ARM64_SYSROOT/usr/include" ]] || \
        die "ARM64 cross-build requires --arm64-sysroot PATH"
    export OPENURMA_ARM64_SYSROOT="$ARM64_SYSROOT"
fi
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
    die "target compiler not found: ${CROSS_COMPILE}gcc"

cmake_cross_args=()
if [[ "$BUILD_MODE" == cross ]]; then
    compiler="$LAB_DIR/tools/aarch64-umdk-cc.py"
    [[ -x "$compiler" ]] || die "UMDK cross-compiler launcher is missing: $compiler"
    cmake_cross_args=(
        -DCMAKE_SYSTEM_NAME=Linux
        -DCMAKE_SYSTEM_PROCESSOR=aarch64
        -DCMAKE_C_COMPILER="$compiler"
        -DCROSS_COMPILE="$compiler"
        -DCMAKE_FIND_ROOT_PATH="$ARM64_SYSROOT"
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
    )
fi

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

echo "Building UMDK $PINNED_UMDK_SHA for $TARGET_ARCH ($BUILD_MODE) with JOBS=$JOBS, stock UDMA=$BUILD_STOCK_UDMA"
rm -rf -- "$BUILD_DIR"

echo "Building gem5 address/instruction pseudo-op library"
scons -C "$GEM5_ROOT/util/m5" \
    "$M5_ABI.CROSS_COMPILE=$CROSS_COMPILE" "build/$M5_ABI/out/libm5.a" >/dev/null
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
        "${cmake_cross_args[@]}" \
        -DUMMU_API_INCLUDE_DIR="$UMMU_DEPS/include" \
        -DUMMU_UAPI_INCLUDE_DIR="$UMMU_DEPS/kernel_headers" \
        -DBUILD_TESTING="$([[ "$BUILD_MODE" == cross ]] && echo OFF || echo ON)" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build "$UMMU_SHIM_BUILD_DIR" --parallel "$JOBS"
    if [[ "$BUILD_MODE" == native ]]; then
        ctest --test-dir "$UMMU_SHIM_BUILD_DIR" --output-on-failure
    else
        echo "Skipping target execution of the ARM64 UMMU self-test during cross-build"
    fi

    # The pinned UMDK build uses plain <ummu_api.h> and -lummu.  Supplying
    # compiler/linker search paths keeps the vendored source unmodified.
    export CPATH="$UMMU_DEPS/include:$UMMU_DEPS/kernel_headers${CPATH:+:$CPATH}"
    export LIBRARY_PATH="$UMMU_SHIM_BUILD_DIR${LIBRARY_PATH:+:$LIBRARY_PATH}"
fi

cmake -S "$UMDK_SRC/src" -B "$BUILD_DIR" \
    "${cmake_cross_args[@]}" \
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
ubagg_provider="$BUILD_DIR/urma/lib/urma/bond/liburma_ubagg.so.0.0.1"
ubagg_cli="$BUILD_DIR/urma/tools/ubagg_cli/ubagg_cli"
libtpsa="$BUILD_DIR/urma/lib/uvs/core/libtpsa.so.0.0.1"
ummu_shim="$UMMU_SHIM_BUILD_DIR/libummu.so.1"

verify_target() {
    local artifact="$1"
    [[ -f "$artifact" ]] || die "missing artifact: $artifact"
    readelf -h "$artifact" | grep -Eq "$TARGET_MACHINE_RE" || {
        file "$artifact" >&2 || true
        die "artifact is not $TARGET_MACHINE_NAME ELF: $artifact"
    }
    printf '%-8s %s\n' "$TARGET_MACHINE_NAME" "$artifact"
}

echo
echo "Verified guest artifacts:"
verify_target "$liburma"
verify_target "$liburma_common"
verify_target "$urma_admin"
verify_target "$urma_perftest"
verify_target "$ubagg_provider"
verify_target "$ubagg_cli"
verify_target "$libtpsa"

if [[ "$BUILD_STOCK_UDMA" == "enable" ]]; then
    verify_target "$stock_udma"
    verify_target "$ummu_shim"
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
