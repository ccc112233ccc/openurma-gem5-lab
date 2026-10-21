#!/usr/bin/env bash
# Build gem5 with only the simulator-neutral UB-HOST front-end.
set -euo pipefail

die() { echo "build_gem5.sh: $*" >&2; exit 2; }
note() { echo "[build-gem5] $*"; }

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
lab_dir="${UBSIM_LAB_ROOT:-$(cd "$script_dir/../.." && pwd)}"
gem5_root="${GEM5_ROOT:-$lab_dir/gem5}"
jobs="${JOBS:-1}"
readonly GEM5_COMMIT=d7a08a8b84b0a393a91e259025c0ab123ad60a6b

[[ "$(uname -s)" == Linux ]] || die "run this on Linux (native Ubuntu or the build container)"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die "JOBS must be a positive integer"
for tool in bash cmp git install make mkdir rsync scons; do
    command -v "$tool" >/dev/null 2>&1 || die "required command is missing: $tool"
done

[[ -d "$gem5_root/.git" ]] || die "gem5 checkout is missing: $gem5_root"
actual=$(git -C "$gem5_root" rev-parse HEAD)
[[ "$actual" == "$GEM5_COMMIT" ]] ||
    die "gem5 is at $actual; expected $GEM5_COMMIT"
activation_patch="$lab_dir/patches/source/gem5/0002-Defer-thread-activation-during-external-fences.patch"
[[ -f "$activation_patch" ]] || die "missing gem5 lifecycle activation patch"
if git -C "$gem5_root" apply --reverse --check "$activation_patch" >/dev/null 2>&1; then
    note "gem5 lifecycle activation patch already applied"
else
    git -C "$gem5_root" apply --check "$activation_patch" ||
        die "gem5 lifecycle activation patch does not apply cleanly"
    git -C "$gem5_root" apply "$activation_patch"
fi
cmp -s "$activation_patch" <(git -C "$gem5_root" diff -- \
    src/sim/system.cc) ||
    die "gem5 lifecycle source differs from the reviewed patch"
exclusive_patch="$lab_dir/patches/source/gem5/0003-Exclusive-store-stream-prefetch.patch"
[[ -f "$exclusive_patch" ]] || die "missing exclusive prefetch patch"
if git -C "$gem5_root" apply --reverse --check "$exclusive_patch" >/dev/null 2>&1; then
    note "gem5 exclusive prefetch patch already applied"
else
    git -C "$gem5_root" apply --check "$exclusive_patch" ||
        die "gem5 exclusive prefetch patch does not apply cleanly"
    git -C "$gem5_root" apply "$exclusive_patch"
fi
cmp -s "$exclusive_patch" <(git -C "$gem5_root" diff -- \
    src/mem/cache/base.cc src/mem/cache/cache.cc src/mem/cache/mshr.cc \
    src/mem/cache/noncoherent_cache.cc \
    src/mem/cache/prefetch/Prefetcher.py \
    src/mem/cache/prefetch/queued.cc src/mem/cache/prefetch/queued.hh \
    src/mem/packet.cc src/mem/packet.hh) ||
    die "gem5 exclusive prefetch source differs from the reviewed patch"
serialization_patch="$lab_dir/patches/source/gem5/0005-Serialize-vector-bool.patch"
[[ -f "$serialization_patch" ]] || die "missing gem5 serialization patch"
if git -C "$gem5_root" apply --reverse --check "$serialization_patch" >/dev/null 2>&1; then
    note "gem5 serialization patch already applied"
else
    git -C "$gem5_root" apply --check "$serialization_patch" ||
        die "gem5 serialization patch does not apply cleanly"
    git -C "$gem5_root" apply "$serialization_patch"
fi
cmp -s "$serialization_patch" <(git -C "$gem5_root" diff -- \
    src/sim/serialize.hh) ||
    die "gem5 serialization source differs from the reviewed patch"
if command -v pgrep >/dev/null 2>&1 &&
   pgrep -af '[s]cons.*build/ARM/gem5[.]opt' >/dev/null 2>&1; then
    die "another gem5 SCons build is already running; wait for it to finish"
fi

adapter_source="$lab_dir/integrations/gem5/ub_host_adapter"
extras_root="$lab_dir/artifacts/gem5-extras"
adapter_extra="$extras_root/ub_host_adapter"
[[ -f "$adapter_source/SConscript" ]] || die "UB-HOST adapter is missing"
mkdir -p "$adapter_extra"
rsync -a --delete "$adapter_source/" "$adapter_extra/"
install -m 0644 \
    "$lab_dir/components/udma-device-sim/simbricks_base_portable.c" \
    "$adapter_extra/simbricks_base_portable.c"

make -C "$gem5_root/util/term" -j"$jobs"
[[ -x "$gem5_root/util/term/m5term" ]] || die "m5term build failed"

# Keep firmware aligned with the pinned CPU model. Older downloaded boot.arm64
# omits CPTR_EL3.ESM: Linux's early SME setup then traps to an empty EL3 vector.
# Build upstream firmware rather than changing the guest kernel or hiding SME.
for tool in aarch64-linux-gnu-gcc aarch64-linux-gnu-ld; do
    command -v "$tool" >/dev/null 2>&1 ||
        die "$tool is required for the ARM64 bootloader"
done
boot_build="$lab_dir/artifacts/gem5-bootloader-arm64"
mkdir -p "$boot_build"
make -C "$gem5_root/system/arm/bootloader/arm64" \
    BUILDDIR="$boot_build" -j"$jobs"
make -C "$gem5_root/system/arm/bootloader/arm64" \
    BUILDDIR="$boot_build" DESTDIR="$lab_dir/system/binaries" install

note "building ARM gem5.opt with the UB-HOST adapter only (JOBS=$jobs)"
(
    cd "$gem5_root"
    export UBSIM_LAB_ROOT="$lab_dir"
    scons --linker=gold --limit-ld-memory-usage build/ARM/gem5.opt \
        EXTRAS="$extras_root" -j"$jobs"
)

[[ -x "$gem5_root/build/ARM/gem5.opt" ]] || die "gem5.opt was not produced"
# SCons can reuse a generated ISA target after a source file is restored by a
# copy that preserves an older timestamp. Reject the known stale DMB decoder
# from a withdrawn experiment before considering the executable usable.
decoder="$gem5_root/build/ARM/arch/arm/generated/decoder-ns.cc.inc"
[[ -f "$decoder" ]] || die "generated ARM decoder is missing"
if grep -Fq 'const uint8_t access = bits(machInst, 9, 8);' "$decoder"; then
    die "generated ARM decoder contains the withdrawn DMB LD/ST experiment"
fi
"$gem5_root/build/ARM/gem5.opt" -h >/dev/null ||
    die "gem5.opt was produced but is not executable"
note "ready: $gem5_root/build/ARM/gem5.opt"
