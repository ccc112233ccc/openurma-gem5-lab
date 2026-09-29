#!/usr/bin/env bash
# Build gem5 with only the simulator-neutral UB-HOST front-end.
set -euo pipefail

die() { echo "build_gem5.sh: $*" >&2; exit 2; }
note() { echo "[build-gem5] $*"; }

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
lab_dir="${OPENURMA_LAB_ROOT:-$(cd "$script_dir/../.." && pwd)}"
gem5_root="${GEM5_ROOT:-$lab_dir/gem5}"
jobs="${JOBS:-1}"
readonly GEM5_COMMIT=d7a08a8b84b0a393a91e259025c0ab123ad60a6b

[[ "$(uname -s)" == Linux ]] || die "run this on Linux (native Ubuntu or the build container)"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die "JOBS must be a positive integer"
for tool in bash git install make mkdir rsync scons; do
    command -v "$tool" >/dev/null 2>&1 || die "required command is missing: $tool"
done

[[ -d "$gem5_root/.git" ]] || die "gem5 checkout is missing: $gem5_root"
actual=$(git -C "$gem5_root" rev-parse HEAD)
[[ "$actual" == "$GEM5_COMMIT" ]] ||
    die "gem5 is at $actual; expected $GEM5_COMMIT"
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

note "building ARM gem5.opt with the UB-HOST adapter only (JOBS=$jobs)"
(
    cd "$gem5_root"
    export OPENURMA_LAB_ROOT="$lab_dir"
    scons --linker=gold --limit-ld-memory-usage build/ARM/gem5.opt \
        EXTRAS="$extras_root" -j"$jobs"
)

[[ -x "$gem5_root/build/ARM/gem5.opt" ]] || die "gem5.opt was not produced"
"$gem5_root/build/ARM/gem5.opt" -h >/dev/null ||
    die "gem5.opt was produced but is not executable"
note "ready: $gem5_root/build/ARM/gem5.opt"
