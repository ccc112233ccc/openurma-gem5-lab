#!/usr/bin/env bash
# Build the patched ARM full-system gem5 and UB-HOST adapter on Apple Silicon.
set -euo pipefail

die() { echo "build_gem5_macos.sh: $*" >&2; exit 2; }

[[ "$(uname -s):$(uname -m)" == Darwin:arm64 ]] ||
    die "requires Apple Silicon macOS"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
lab="${UBSIM_LAB_ROOT:-$(cd "$script_dir/../.." && pwd)}"
gem5_root="${GEM5_ROOT:-$lab/gem5}"
jobs="${JOBS:-4}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die "JOBS must be a positive integer"
for tool in python3 git rsync install pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || die "missing command: $tool"
done
[[ "$(git -C "$gem5_root" rev-parse HEAD)" == \
   d7a08a8b84b0a393a91e259025c0ab123ad60a6b ]] ||
    die "gem5 checkout does not match the pinned commit"

deps="$lab/artifacts/native-macos-python-deps"
mkdir -p "$deps"
export PYTHONPATH="$gem5_root/ext/Kconfiglib/import:$deps${PYTHONPATH:+:$PYTHONPATH}"
if ! python3 -c 'import SCons, yaml; assert SCons.__version__ == "4.8.1"' \
        >/dev/null 2>&1; then
    python3 -m pip install --upgrade --target "$deps" \
        'scons==4.8.1' 'PyYAML>=6,<7'
fi

for patch in \
    "$lab/patches/source/gem5/0002-Defer-thread-activation-during-external-fences.patch" \
    "$lab/patches/source/gem5/0003-Exclusive-store-stream-prefetch.patch" \
    "$lab/patches/source/gem5/0004-macos-clang-compatibility.patch" \
    "$lab/patches/source/gem5/0005-Serialize-vector-bool.patch"; do
    [[ -f "$patch" ]] || die "missing gem5 patch: $patch"
    if ! git -C "$gem5_root" apply --reverse --check "$patch" >/dev/null 2>&1; then
        git -C "$gem5_root" apply --check "$patch" ||
            die "gem5 patch does not apply: $patch"
        git -C "$gem5_root" apply "$patch"
    fi
done

adapter_source="$lab/integrations/gem5/ub_host_adapter"
extras="$lab/artifacts/gem5-extras"
[[ -f "$adapter_source/SConscript" ]] || die "UB-HOST adapter source is missing"
mkdir -p "$extras/ub_host_adapter"
rsync -a --delete "$adapter_source/" "$extras/ub_host_adapter/"
install -m 0644 "$lab/components/udma-device-sim/simbricks_base_portable.c" \
    "$extras/ub_host_adapter/simbricks_base_portable.c"

export UBSIM_LAB_ROOT="$lab"
export CCFLAGS_EXTRA="${CCFLAGS_EXTRA:+$CCFLAGS_EXTRA }-Wno-error=vla-cxx-extension -Wno-deprecated-literal-operator"
export GEM5PY_CCFLAGS_EXTRA="${GEM5PY_CCFLAGS_EXTRA:+$GEM5PY_CCFLAGS_EXTRA }-Wno-error=vla-cxx-extension -Wno-deprecated-literal-operator"
(
    cd "$gem5_root"
    if [[ ! -f build/ARM_MAC/gem5.build/config ]]; then
        python3 -m SCons defconfig build/ARM_MAC build_opts/ARM
    fi
    python3 -m SCons build/ARM_MAC/gem5.opt EXTRAS="$extras" -j"$jobs"
)

binary="$gem5_root/build/ARM_MAC/gem5.opt"
[[ -x "$binary" ]] || die "native gem5 executable was not produced"
"$binary" -h >/dev/null || die "native gem5 executable cannot start"
echo "Native gem5 ready: $binary"
