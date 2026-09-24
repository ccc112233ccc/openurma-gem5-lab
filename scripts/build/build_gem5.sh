#!/usr/bin/env bash
# Rebuild the exact OpenURMA-enabled gem5 used by this lab.
# Run this script on Linux, either natively or inside the Ubuntu build container.
set -euo pipefail

die() {
    echo "build_gem5.sh: $*" >&2
    exit 2
}

note() {
    echo "[build-gem5] $*"
}

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
lab_dir="${OPENURMA_LAB_ROOT:-$(cd "$script_dir/../.." && pwd)}"
gem5_root="${GEM5_ROOT:-$lab_dir/gem5}"
openurma_root="${OPENURMA_ROOT:-$lab_dir/sources/OpenURMA}"
openclicknp_root="${OPENCLICKNP_ROOT:-$lab_dir/sources/OpenClickNP}"
jobs="${JOBS:-1}"

# These are repository object IDs, not moving branch or tag names.
readonly GEM5_COMMIT=54c9d7cc2c6c3cb3bc215716ba1e632df18d84e4
readonly OPENURMA_COMMIT=0381d0b61a17c3614446b13a4e0d842045d69c01
readonly OPENCLICKNP_COMMIT=c1c6acc58032a1894507d88659b3cca668b0e1a5
readonly UMDK_COMMIT=f84b90b8ddd8173b851334f55d332783d248bfc7

[[ "$(uname -s)" == Linux ]] ||
    die "run this on Linux (native Ubuntu or the build container)"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die "JOBS must be a positive integer"

for tool in ar bash cmp find g++ git install make mkdir readlink rsync scons sed; do
    command -v "$tool" >/dev/null 2>&1 || die "required command is missing: $tool"
done

require_commit() {
    local label=$1
    local tree=$2
    local expected=$3
    local actual top

    [[ -d "$tree" ]] || die "$label checkout is missing: $tree"
    git -C "$tree" rev-parse --is-inside-work-tree >/dev/null 2>&1 ||
        die "$label is not a Git checkout: $tree"
    top=$(git -C "$tree" rev-parse --show-toplevel)
    [[ "$(readlink -f "$top")" == "$(readlink -f "$tree")" ]] ||
        die "$label path is not its repository root: $tree"
    actual=$(git -C "$tree" rev-parse HEAD)
    [[ "$actual" == "$expected" ]] ||
        die "$label is at $actual; expected $expected"
    note "$label commit verified: $actual"
}

install_fixed_topology() {
    local source=$1
    local destination=$2

    [[ -f "$source" ]] || die "fixed topology is missing: $source"
    if ! cmp -s "$source" "$destination"; then
        install -D -m 0644 "$source" "$destination"
        note "installed fixed topology: $destination"
    else
        note "fixed topology already installed: $destination"
    fi
    cmp -s "$source" "$destination" ||
        die "fixed topology verification failed: $destination"
}

build_isolated_tlm_facade() {
    local gem5_systemc="$gem5_root/src/systemc/ext/systemc_home/include"
    local generated="$openurma_root/build/openurma_gen"
    local outdir="$openurma_root/build/sc_gem5_tlm"

    [[ -d "$gem5_systemc" ]] ||
        die "gem5 SystemC headers are missing: $gem5_systemc"
    mkdir -p "$outdir"

    # NICTopologySC.cc and the legacy NIC_TLM facade both include the complete
    # generated topology.  Give the facade's private copy its own namespace so
    # the two implementations can coexist without ODR collisions, while the
    # public openurma::sc::NIC_TLM ABI remains unchanged.
    g++ -std=c++17 -O2 -DSC_INCLUDE_DYNAMIC_PROCESSES \
        -Dtlm_topo=facade_tlm_topo -fPIC \
        -Wall -Wno-unused-variable -Wno-unused-but-set-variable \
        -Wno-unused-label -Wno-unused-function \
        -I "$gem5_systemc" \
        -I "$openclicknp_root/runtime/include" \
        -I "$openurma_root/runtime/openurma/include" \
        -I "$generated/systemc" \
        -include "openurma/ub_flit.hpp" \
        -c "$openurma_root/runtime/openurma/src/openurma_tlm_facade.cpp" \
        -o "$outdir/openurma_tlm_facade.o"
    ar rcs "$outdir/libopenurma_sc_tlm.a" \
        "$outdir/openurma_tlm_facade.o"
    note "built isolated gem5-ABI facade: $outdir/libopenurma_sc_tlm.a"
}

umdk_root="$openurma_root/integration/umdk/vendor/umdk"
require_commit gem5 "$gem5_root" "$GEM5_COMMIT"
require_commit OpenURMA "$openurma_root" "$OPENURMA_COMMIT"
require_commit OpenClickNP "$openclicknp_root" "$OPENCLICKNP_COMMIT"
require_commit UMDK "$umdk_root" "$UMDK_COMMIT"

# Avoid two build processes mutating the same source and output trees. The
# script only reports the conflict; it never signals or stops the other build.
if command -v pgrep >/dev/null 2>&1 &&
   pgrep -af '[s]cons.*build/ARM/gem5[.]opt' >/dev/null 2>&1; then
    die "another gem5 SCons build is already running; wait for it to finish"
fi

scaffold_src="$openurma_root/eval/twonode/gem5_scaffold/src"
[[ -f "$scaffold_src/SConscript" ]] || die "OpenURMA SConscript is missing"

# The upstream experiment scaffold contains historical /home/ubuntu paths.
# Create a generated build view with those paths rewritten instead of requiring
# root-owned symlinks on every native Linux builder. The source checkout stays
# unchanged and EXTRAS registers this view directly with SCons.
scaffold_extra="$lab_dir/artifacts/gem5-openurma-scaffold"
mkdir -p "$scaffold_extra"
rsync -a --delete "$scaffold_src/" "$scaffold_extra/"
find "$scaffold_extra" -type f \
    -exec sed -i \
        -e "s#/home/ubuntu/OpenURMA#$openurma_root#g" \
        -e "s#/home/ubuntu/OpenClickNP#$openclicknp_root#g" \
        -e "s#/home/ubuntu/gem5#$gem5_root#g" {} +
[[ -f "$scaffold_extra/SConscript" ]] ||
    die "OpenURMA SConscript is not reachable through $scaffold_extra"

patch_file="$openurma_root/eval/twonode/gem5_scaffold/patches/gem5_sc_deschedule.patch"
[[ -f "$patch_file" ]] || die "gem5 SystemC deschedule patch is missing"
if git -C "$gem5_root" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
    note "gem5 SystemC deschedule patch already applied"
elif git -C "$gem5_root" apply --check "$patch_file" >/dev/null 2>&1; then
    git -C "$gem5_root" apply "$patch_file"
    note "applied gem5 SystemC deschedule patch"
else
    die "deschedule patch is neither cleanly applicable nor already applied"
fi

patch_dir="$openurma_root/eval/twonode/gem5_scaffold/patches"
install_fixed_topology \
    "$patch_dir/openurma_gen_fixed/topology_tlm.cpp" \
    "$openurma_root/build/openurma_gen/systemc/topology_tlm.cpp"
install_fixed_topology \
    "$patch_dir/openroce_gen_fixed/topology_tlm.cpp" \
    "$openurma_root/build/openroce_gen/systemc/topology_tlm.cpp"

note "building the library against gem5's embedded SystemC ABI"
build_isolated_tlm_facade
sc_abi_lib="$openurma_root/build/sc_gem5_tlm/libopenurma_sc_tlm.a"
[[ -s "$sc_abi_lib" ]] || die "gem5-ABI library was not produced: $sc_abi_lib"

# m5term is small and is needed to attach to the PL011 UART on TCP port 3456.
make -C "$gem5_root/util/term" -j"$jobs"
[[ -x "$gem5_root/util/term/m5term" ]] || die "m5term build failed"

note "building gem5.opt with gold low-memory linking, OpenURMA EXTRAS, and JOBS=$jobs"
(
    cd "$gem5_root"
    export OPENURMA_ROOT="$openurma_root"
    export OPENCLICKNP_ROOT="$openclicknp_root"
    # SCons deliberately does not follow src/ directory symlinks while walking
    # the built-in source tree.  Keep the canonical symlink for the checked-in
    # configuration's imports, but register the scaffold explicitly as EXTRAS
    # so its SConscript/SimObjects are compiled into gem5.opt.
    scons --linker=gold --limit-ld-memory-usage build/ARM/gem5.opt \
        USE_SYSTEMC=1 \
        EXTRAS="$scaffold_extra" \
        -j"$jobs"
)

[[ -x "$gem5_root/build/ARM/gem5.opt" ]] || die "gem5.opt was not produced"
"$gem5_root/build/ARM/gem5.opt" -h >/dev/null ||
    die "gem5.opt was produced but is not executable"
note "ready: $gem5_root/build/ARM/gem5.opt"
