#!/usr/bin/env bash
# Fetch public upstream baselines and overlay the exact lab commits from bundles.
set -euo pipefail

lab="${OPENURMA_LAB_ROOT:-/workspace/openurma-gem5-lab}"
sources="$lab/sources"
kernel_root="${KSRC:-/opt/openurma-gem5-lab/oe66}"

readonly GEM5_URL=https://github.com/gem5/gem5.git
readonly GEM5_BASE=b1a44b89c7bae73fae2dc547bc1f871452075b85
readonly GEM5_TARGET=724651433c9bdee2c7f0484ab85b9620b0810993
readonly OPENURMA_URL=https://github.com/bojieli/OpenURMA.git
readonly OPENURMA_BASE=0ae5dce300154d761f97095864bda0cf2546b265
readonly OPENURMA_TARGET=231b43387ec5e7b34430562f6def90d68b584b3c
readonly UMDK_URL=https://gitee.com/openeuler/umdk.git
readonly UMDK_BASE=4eab3e4ad170b06bfe5d5c1014341e81edb9bf58
readonly UMDK_TARGET=f84b90b8ddd8173b851334f55d332783d248bfc7
readonly OPENCLICKNP_URL=https://github.com/bojieli/OpenClickNP.git
readonly OPENCLICKNP_TARGET=c1c6acc58032a1894507d88659b3cca668b0e1a5
readonly UMMU_URL=https://gitee.com/openeuler/ummu.git
readonly UMMU_TARGET=f1930d006e08bbe96dfa6fa037ff8a386f535425
readonly KERNEL_URL=https://gitee.com/openeuler/kernel.git
readonly KERNEL_TARGET=5078a3a23a1e1825ec136485173ec98668cdd640
readonly ARM_TARBALL_SHA=1e49513d680ccd3c0dd32cbfcf7d3421b4bf8bb35f7c8467de9c863cf7cc1344
readonly ARM_TARBALL_URL=https://dist.gem5.org/dist/v22-0/arm/aarch-system-20220707.tar.bz2

note() { echo "[sources] $*"; }
die() { echo "[sources] ERROR: $*" >&2; exit 1; }

fetch_base() {
    local name=$1 url=$2 dir=$3 commit=$4
    if [[ ! -e "$dir/.git" ]]; then
        [[ ! -e "$dir" ]] || [[ -z "$(find "$dir" -mindepth 1 -maxdepth 1 -print -quit)" ]] || \
            die "$name target exists but is not a Git checkout: $dir"
        mkdir -p "$dir"
        git -C "$dir" init -q
        git -C "$dir" remote add origin "$url"
    fi
    git -C "$dir" cat-file -e "$commit^{commit}" 2>/dev/null || \
        git -C "$dir" fetch --no-tags --depth=1 origin "$commit"
    git -C "$dir" checkout -q --detach "$commit"
}

fetch_with_bundle() {
    local name=$1 url=$2 dir=$3 base=$4 target=$5 bundle=$6
    if [[ -e "$dir/.git" ]] && [[ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" == "$target" ]]; then
        note "$name already at $target"
        return
    fi
    fetch_base "$name" "$url" "$dir" "$base"
    git -C "$dir" fetch -q "$bundle" HEAD
    git -C "$dir" checkout -q --detach "$target"
    [[ "$(git -C "$dir" rev-parse HEAD)" == "$target" ]] || die "$name target verification failed"
    note "$name ready at $target"
}

fetch_plain() {
    local name=$1 url=$2 dir=$3 target=$4
    if [[ -e "$dir/.git" ]] && [[ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" == "$target" ]]; then
        note "$name already at $target"
        return
    fi
    fetch_base "$name" "$url" "$dir" "$target"
    note "$name ready at $target"
}

cd "$lab"
sha256sum -c patches/source/BUNDLE_SHA256SUMS
mkdir -p "$sources" "$lab/deps" "$lab/downloads" "$lab/system"

fetch_with_bundle gem5 "$GEM5_URL" "$lab/gem5" "$GEM5_BASE" "$GEM5_TARGET" \
    "$lab/patches/source/gem5.bundle"
fetch_with_bundle OpenURMA "$OPENURMA_URL" "$sources/OpenURMA" \
    "$OPENURMA_BASE" "$OPENURMA_TARGET" "$lab/patches/source/openurma.bundle"

# The parent repository records UMDK as a gitlink. Populate that path from the
# official UMDK upstream, then fetch the exact simulator instrumentation commits.
umdk="$sources/OpenURMA/integration/umdk/vendor/umdk"
fetch_with_bundle UMDK "$UMDK_URL" "$umdk" "$UMDK_BASE" "$UMDK_TARGET" \
    "$lab/patches/source/umdk.bundle"
fetch_plain OpenClickNP "$OPENCLICKNP_URL" "$sources/OpenClickNP" "$OPENCLICKNP_TARGET"
fetch_plain UMMU "$UMMU_URL" "$lab/deps/ummu" "$UMMU_TARGET"
fetch_plain OLK-6.6 "$KERNEL_URL" "$kernel_root" "$KERNEL_TARGET"

archive="$lab/downloads/aarch-system-20220707.tar.bz2"
if [[ ! -f "$archive" ]] || ! echo "$ARM_TARBALL_SHA  $archive" | sha256sum -c - >/dev/null 2>&1; then
    note "downloading gem5 ARM firmware bundle"
    curl -fL --retry 5 --retry-delay 2 "$ARM_TARBALL_URL" -o "$archive.part"
    echo "$ARM_TARBALL_SHA  $archive.part" | sha256sum -c -
    mv "$archive.part" "$archive"
fi
if [[ ! -s "$lab/system/binaries/boot.arm64" || ! -s "$lab/system/binaries/boot.arm" ]]; then
    tar -xjf "$archive" -C "$lab/system"
fi

note "all source and firmware inputs passed revision checks"
