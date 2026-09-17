#!/usr/bin/env bash
# Build an interactive ARM64 initramfs for OpenURMA Tier-G.
#
# This script intentionally does not download anything or modify source trees.
# It consumes an already-built OLK-6.6 tree and an already cross-built UMDK
# tree, persists the selected boot artifacts in the workspace, builds the small
# OpenURMA userspace provider/smoke binary from the checked-out sources, and
# packages those artifacts with a static ARM64 BusyBox.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAB_DIR="${LAB_DIR:-$SCRIPT_DIR}"
OPENURMA_ROOT="${OPENURMA_ROOT:-$LAB_DIR/../OpenURMA}"
GEM5_ROOT="${GEM5_ROOT:-$LAB_DIR/gem5}"
KSRC="${KSRC:-}"
ARM_BUILD="${ARM_BUILD:-}"
BUSYBOX_ARM64="${BUSYBOX_ARM64:-}"
OUT="${OUT:-$LAB_DIR/out/openurma-interactive.cpio.gz}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
EXTRA_BINS="${EXTRA_BINS:-}"
EXTRA_MODULES="${EXTRA_MODULES:-}"
STOCK_UDMA_PROVIDER="${STOCK_UDMA_PROVIDER:-}"
UMMU_SHIM="${UMMU_SHIM:-}"
KERNEL_BUNDLE_DIR="${KERNEL_BUNDLE_DIR:-$LAB_DIR/artifacts/kernel}"

usage() {
    cat <<'EOF'
Usage:
  KSRC=/path/to/olk-6.6 \
  ARM_BUILD=/path/to/umdk-arm-build \
  BUSYBOX_ARM64=/path/to/static-arm64-busybox \
    ./build-interactive-initramfs.sh

Optional environment:
  OPENURMA_ROOT  OpenURMA checkout (default: ../OpenURMA)
  OUT            output .cpio.gz path
  CROSS_COMPILE  tool prefix (default: aarch64-linux-gnu-)
  EXTRA_BINS     space-separated extra ARM64 executables to place in /usr/bin
  EXTRA_MODULES  space-separated extra kernel modules to place in /lib/modules
  STOCK_UDMA_PROVIDER
                 stock liburma-udma.so to package alongside the legacy
                 OpenURMA provider (auto-detected in ARM_BUILD when present)
  UMMU_SHIM      simulation libummu.so.1 paired with STOCK_UDMA_PROVIDER
                 (auto-detected next to ARM_BUILD when present)
  KERNEL_BUNDLE_DIR
                 persistent workspace bundle for the exact vmlinux/in-tree
                 modules packaged in the image (default: artifacts/kernel)
  UBCORE_KO, UBURMA_KO, IPV6_KO, OPENURMA_KO
                 explicit module paths (normally auto-detected)

The kernel tree must already contain vmlinux, ipv6.ko, ubcore.ko and uburma.ko.
No source tree is changed and this script performs no network access.
EOF
}

die() { printf 'error: %s\n' "$*" >&2; exit 2; }
note() { printf '[initramfs] %s\n' "$*"; }

[[ "${1:-}" != "-h" && "${1:-}" != "--help" ]] || { usage; exit 0; }
[[ -n "$KSRC" ]] || { usage >&2; die 'KSRC is required'; }
[[ -n "$ARM_BUILD" ]] || { usage >&2; die 'ARM_BUILD is required'; }
[[ -n "$BUSYBOX_ARM64" ]] || { usage >&2; die 'BUSYBOX_ARM64 is required'; }
# Keep sibling auto-detection stable when callers spell ARM_BUILD with one or
# more trailing slashes ("$ARM_BUILD-ummu-shim" would otherwise point inside
# the build tree instead of beside it).
while [[ "$ARM_BUILD" == */ && "$ARM_BUILD" != "/" ]]; do
    ARM_BUILD="${ARM_BUILD%/}"
done
[[ -d "$OPENURMA_ROOT/integration/umdk" ]] || die "not an OpenURMA checkout: $OPENURMA_ROOT"
[[ -f "$GEM5_ROOT/include/gem5/m5ops.h" ]] || die "not a gem5 checkout: $GEM5_ROOT"
[[ -f "$KSRC/Makefile" && -f "$KSRC/vmlinux" ]] || die "KSRC is not a built kernel tree: $KSRC"
[[ -f "$BUSYBOX_ARM64" ]] || die "BusyBox not found: $BUSYBOX_ARM64"

CC="${CROSS_COMPILE}gcc"
READELF="${CROSS_COMPILE}readelf"
command -v "$CC" >/dev/null || die "cross compiler not found: $CC"
command -v "$READELF" >/dev/null || die "readelf not found: $READELF"
command -v cpio >/dev/null || die 'cpio is required'
command -v gzip >/dev/null || die 'gzip is required'
command -v sha256sum >/dev/null || die 'sha256sum is required'

is_arm64_elf() {
    "$READELF" -h "$1" 2>/dev/null | grep -q 'Machine:.*AArch64'
}

is_static_elf() {
    ! "$READELF" -l "$1" 2>/dev/null | grep -q 'Requesting program interpreter'
}

is_arm64_elf "$BUSYBOX_ARM64" || die 'BUSYBOX_ARM64 is not an AArch64 ELF binary'
is_static_elf "$BUSYBOX_ARM64" || die 'BUSYBOX_ARM64 must be statically linked'

REQUIRED_APPLETS='sh ash mount umount mkdir ln ls cat echo dmesg insmod rmmod lsmod ip hostname uname ps grep sed awk sleep sync poweroff reboot halt setsid cttyhack chmod touch taskset'
if busybox_applets="$($BUSYBOX_ARM64 --list 2>/dev/null)"; then
    for applet in $REQUIRED_APPLETS; do
        grep -qx "$applet" <<<"$busybox_applets" ||
            die "BUSYBOX_ARM64 does not provide the required '$applet' applet"
    done
fi

kernel_release="$(make -s -C "$KSRC" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" kernelrelease)"
case "$kernel_release" in
    6.6*) ;;
    *) die "kernel release '$kernel_release' is not OLK-6.6; current UMDK uses the TLV uburma ABI" ;;
esac

find_one() {
    local label="$1"
    shift
    local p
    for p in "$@"; do
        if [[ -f "$p" ]]; then printf '%s\n' "$p"; return 0; fi
    done
    die "$label not found; checked: $*"
}

UBCORE_KO="${UBCORE_KO:-$(find_one ubcore.ko \
    "$KSRC/drivers/ub/urma/ubcore/ubcore.ko" \
    "$KSRC/drivers/ub/ubcore/ubcore.ko")}"
UBURMA_KO="${UBURMA_KO:-$(find_one uburma.ko \
    "$KSRC/drivers/ub/urma/uburma/uburma.ko" \
    "$KSRC/drivers/ub/uburma/uburma.ko")}"
IPV6_KO="${IPV6_KO:-$(find_one ipv6.ko "$KSRC/net/ipv6/ipv6.ko")}"
OPENURMA_KO="${OPENURMA_KO:-$(find_one openurma_ubcore.ko \
    "$OPENURMA_ROOT/integration/umdk/kmod/openurma_ubcore.ko")}"

LIBURMA="$(find_one liburma.so.0 \
    "$ARM_BUILD/urma/lib/urma/core/liburma.so.0" \
    "$ARM_BUILD/urma/lib/urma/core/liburma.so")"
LIBCOMMON="$(find_one liburma_common.so.0 \
    "$ARM_BUILD/urma/common/liburma_common.so.0" \
    "$ARM_BUILD/urma/common/liburma_common.so")"
URMA_ADMIN="$(find_one urma_admin \
    "$ARM_BUILD/urma/tools/urma_admin/urma_admin")"
URMA_PERFTEST="$(find_one urma_perftest \
    "$ARM_BUILD/urma/tools/urma_perftest/urma_perftest")"

# A BUILD_STOCK_UDMA=enable build places the untouched official provider in
# ARM_BUILD and its simulation-only UMMU ABI dependency in a sibling build
# directory.  Package the pair when available, but keep old legacy-only build
# trees valid.  Supplying exactly one is an error: loading the stock provider
# with a host/full UMMU library would silently change the simulation contract.
if [[ -z "$STOCK_UDMA_PROVIDER" && -f "$ARM_BUILD/urma/hw/udma/liburma-udma.so" ]]; then
    STOCK_UDMA_PROVIDER="$ARM_BUILD/urma/hw/udma/liburma-udma.so"
fi
if [[ -z "$UMMU_SHIM" && -f "${ARM_BUILD}-ummu-shim/libummu.so.1" ]]; then
    UMMU_SHIM="${ARM_BUILD}-ummu-shim/libummu.so.1"
fi
if [[ -n "$STOCK_UDMA_PROVIDER" || -n "$UMMU_SHIM" ]]; then
    [[ -f "$STOCK_UDMA_PROVIDER" ]] ||
        die "stock UDMA provider not found: $STOCK_UDMA_PROVIDER"
    [[ -f "$UMMU_SHIM" ]] || die "simulation UMMU shim not found: $UMMU_SHIM"
fi

for f in "$KSRC/vmlinux" "$UBCORE_KO" "$UBURMA_KO" "$IPV6_KO" "$OPENURMA_KO" \
         "$LIBURMA" "$LIBCOMMON" "$URMA_ADMIN" "$URMA_PERFTEST"; do
    is_arm64_elf "$f" || die "not an AArch64 ELF artifact: $f"
done
if [[ -n "$STOCK_UDMA_PROVIDER" ]]; then
    is_arm64_elf "$STOCK_UDMA_PROVIDER" ||
        die "not an AArch64 ELF artifact: $STOCK_UDMA_PROVIDER"
    is_arm64_elf "$UMMU_SHIM" || die "not an AArch64 ELF artifact: $UMMU_SHIM"
fi

module_release() {
    "$READELF" -p .modinfo "$1" 2>/dev/null |
        sed -n 's/.*vermagic=\([^[:space:]]*\).*/\1/p' | head -n 1
}
for ko in "$UBCORE_KO" "$UBURMA_KO" "$IPV6_KO" "$OPENURMA_KO"; do
    rel="$(module_release "$ko")"
    [[ -n "$rel" ]] || die "cannot read vermagic from $ko"
    [[ "$rel" == "$kernel_release" ]] ||
        die "module/kernel ABI mismatch: $(basename "$ko")=$rel, kernel=$kernel_release"
done
for ko in $EXTRA_MODULES; do
    [[ -f "$ko" ]] || die "EXTRA_MODULES entry not found: $ko"
    rel="$(module_release "$ko")"
    [[ -n "$rel" ]] || die "cannot read vermagic from $ko"
    [[ "$rel" == "$kernel_release" ]] ||
        die "module/kernel ABI mismatch: $(basename "$ko")=$rel, kernel=$kernel_release"
done

# KSRC normally lives on the container's case-sensitive /opt filesystem, which
# is deliberately not part of the portable runtime.  Persist the exact kernel
# and in-tree modules selected above under the workspace before packaging, then
# use those copies for both the initramfs and its manifest.  The temporary-file
# rename keeps every individual artifact readable while a bundle is refreshed;
# an identical destination is left untouched.
persist_kernel_artifact() {
    local src="$1"
    local dst="$2"
    local src_hash dst_hash tmp
    src_hash="$(sha256sum "$src" | awk '{print $1}')"
    # A content-identical symlink may still lead back into /opt, so only a
    # regular workspace file is eligible for the no-copy fast path.
    if [[ -f "$dst" && ! -L "$dst" ]]; then
        dst_hash="$(sha256sum "$dst" | awk '{print $1}')"
        [[ "$dst_hash" == "$src_hash" ]] && return 0
    fi
    mkdir -p "$(dirname "$dst")"
    tmp="$(mktemp "$(dirname "$dst")/.$(basename "$dst").XXXXXX")"
    if ! cp -L "$src" "$tmp"; then
        rm -f "$tmp"
        return 1
    fi
    chmod 0644 "$tmp"
    dst_hash="$(sha256sum "$tmp" | awk '{print $1}')"
    if [[ "$dst_hash" != "$src_hash" ]]; then
        rm -f "$tmp"
        die "workspace artifact copy changed content: $src -> $dst"
    fi
    mv -f "$tmp" "$dst"
}

persist_kernel_artifact "$KSRC/vmlinux" "$KERNEL_BUNDLE_DIR/vmlinux"
persist_kernel_artifact "$IPV6_KO" "$KERNEL_BUNDLE_DIR/modules/ipv6.ko"
persist_kernel_artifact "$UBCORE_KO" "$KERNEL_BUNDLE_DIR/modules/ubcore.ko"
persist_kernel_artifact "$UBURMA_KO" "$KERNEL_BUNDLE_DIR/modules/uburma.ko"
printf '%s\n' "$kernel_release" > "$KERNEL_BUNDLE_DIR/kernelrelease.txt"

VMLINUX="$KERNEL_BUNDLE_DIR/vmlinux"
IPV6_KO="$KERNEL_BUNDLE_DIR/modules/ipv6.ko"
UBCORE_KO="$KERNEL_BUNDLE_DIR/modules/ubcore.ko"
UBURMA_KO="$KERNEL_BUNDLE_DIR/modules/uburma.ko"

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/openurma-interactive.XXXXXX")"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT
mkdir -p "$STAGE"/{bin,sbin,etc,proc,sys,dev/pts,run,tmp,root,usr/bin,usr/sbin,usr/local/bin,lib/modules,lib/urma}
chmod 1777 "$STAGE/tmp"

cp -L "$BUSYBOX_ARM64" "$STAGE/bin/busybox"
chmod 0755 "$STAGE/bin/busybox"
for applet in sh ash mount umount mkdir mknod ln ls cat echo printf dmesg insmod rmmod lsmod \
              ip hostname uname ps grep sed awk sleep sync poweroff reboot halt setsid cttyhack \
              stty clear reset env id head tail hexdump devmem find chmod touch taskset; do
    ln -s busybox "$STAGE/bin/$applet"
done
ln -s /bin/busybox "$STAGE/sbin/init"

cp -L "$IPV6_KO" "$UBCORE_KO" "$UBURMA_KO" "$OPENURMA_KO" "$STAGE/lib/modules/"
for ko in $EXTRA_MODULES; do
    cp -L "$ko" "$STAGE/lib/modules/"
done
cp -L "$LIBURMA" "$STAGE/lib/liburma.so.0"
cp -L "$LIBCOMMON" "$STAGE/lib/liburma_common.so.0"
cp -L "$URMA_ADMIN" "$STAGE/usr/bin/urma_admin"
cp -L "$URMA_PERFTEST" "$STAGE/usr/bin/urma_perftest"
chmod 0755 "$STAGE/usr/bin/urma_admin" "$STAGE/usr/bin/urma_perftest"

# A tiny honest wrapper around the collective dist-gem5 pseudo operation.
# It is intentionally named "toggle", not "on": invoking it a second time
# collectively disables synchronization, while invoking it on only one node
# quiesces that node waiting for its peer.
note "building ARM64 dist-sync pseudo-op helper"
scons -C "$GEM5_ROOT/util/m5" \
    "arm64.CROSS_COMPILE=$CROSS_COMPILE" build/arm64/out/libm5.a >/dev/null
M5_LIB="$GEM5_ROOT/util/m5/build/arm64/out/libm5.a"
M5OPS_DISPATCH_HEADER="$LAB_DIR/tools/ou-m5ops.h"
[[ -f "$M5OPS_DISPATCH_HEADER" ]] || die "missing m5ops dispatcher: $M5OPS_DISPATCH_HEADER"
"$CC" -O2 -static -Wall -I"$GEM5_ROOT/include" \
    -I"$GEM5_ROOT/util/m5/src" -I"$LAB_DIR/tools" \
    -o "$STAGE/usr/bin/ou-dist-sync" "$LAB_DIR/tools/ou-dist-sync.c" "$M5_LIB"
is_arm64_elf "$STAGE/usr/bin/ou-dist-sync" || die "ou-dist-sync is not AArch64"
is_static_elf "$STAGE/usr/bin/ou-dist-sync" || die "ou-dist-sync is not static"

# A distinct switchcpu pseudo-op lets the host configuration replace the
# boot-fast AtomicSimpleCPUs with the configured ArmO3 CPUs at an explicit
# guest-visible boundary.  It is not a timer and does not guess when boot is
# complete.
"$CC" -O2 -static -Wall -I"$GEM5_ROOT/include" \
    -I"$GEM5_ROOT/util/m5/src" -I"$LAB_DIR/tools" \
    -o "$STAGE/usr/bin/ou-cpu-switch-op" \
    "$LAB_DIR/tools/ou-cpu-switch.c" "$M5_LIB"
is_arm64_elf "$STAGE/usr/bin/ou-cpu-switch-op" || \
    die "ou-cpu-switch-op is not AArch64"
is_static_elf "$STAGE/usr/bin/ou-cpu-switch-op" || \
    die "ou-cpu-switch-op is not static"

# Build the Tier-G provider that liburma dlopens from <liburma-dir>/urma/.
UMDK_SRC="$OPENURMA_ROOT/integration/umdk/vendor/umdk/src"
URMA_INC="$UMDK_SRC/urma/lib/urma/core/include"
COMMON_INC="$UMDK_SRC/urma/common/include"
PROVIDER_SRC="$OPENURMA_ROOT/integration/umdk/provider/openurma_provider_kernel.c"
"$CC" -O2 -fPIC -shared -Wall \
    -Wl,-soname,liburma_openurma.so -Wl,-rpath,/lib \
    -I"$URMA_INC" -I"$COMMON_INC" \
    -L"$(dirname "$LIBURMA")" -L"$(dirname "$LIBCOMMON")" \
    -o "$STAGE/lib/urma/liburma_openurma.so" "$PROVIDER_SRC" \
    -Wl,--no-as-needed -lurma -lurma_common -lpthread -ldl
chmod 0755 "$STAGE/lib/urma/liburma_openurma.so"

available_providers=legacy
if [[ -n "$STOCK_UDMA_PROVIDER" ]]; then
    cp -L "$STOCK_UDMA_PROVIDER" "$STAGE/lib/urma/liburma-udma.so"
    cp -L "$UMMU_SHIM" "$STAGE/lib/libummu.so.1"
    chmod 0755 "$STAGE/lib/urma/liburma-udma.so" "$STAGE/lib/libummu.so.1"
    available_providers="$available_providers udma"
fi
printf '%s\n' "$available_providers" > "$STAGE/etc/openurma-available-providers"

# k_smoke is the smallest useful command-line exercise of the complete
# userspace -> TLV ioctl -> kernel provider -> NIC path.
"$CC" -O2 -Wall -Wl,-rpath,/lib \
    -I"$URMA_INC" -I"$COMMON_INC" \
    -L"$(dirname "$LIBURMA")" -L"$(dirname "$LIBCOMMON")" \
    -o "$STAGE/usr/bin/k_smoke" \
    "$OPENURMA_ROOT/integration/umdk/tests/k_smoke.c" \
    -Wl,--no-as-needed -lurma -lurma_common -lpthread -ldl

# Full data-plane coverage: WRITE/READ, atomics, SEND/RECV, immediate data and
# an out-of-bounds error completion. The program validates both CQEs and bytes.
"$CC" -O2 -Wall -Wl,-rpath,/lib \
    -I"$URMA_INC" -I"$COMMON_INC" \
    -L"$(dirname "$LIBURMA")" -L"$(dirname "$LIBCOMMON")" \
    -o "$STAGE/usr/bin/k_dataplane" \
    "$OPENURMA_ROOT/integration/umdk/tests/k_dataplane.c" \
    -Wl,--no-as-needed -lurma -lurma_common -lpthread -ldl

# Deterministic two-process proof: node1 issues WRITE_IMM and node0 verifies
# bytes in its independent guest memory after the payload crosses the peer ring.
"$CC" -O2 -Wall -Wl,-rpath,/lib \
    -I"$URMA_INC" -I"$COMMON_INC" \
    -L"$(dirname "$LIBURMA")" -L"$(dirname "$LIBCOMMON")" \
    -o "$STAGE/usr/bin/twonode_write" \
    "$OPENURMA_ROOT/integration/umdk/apps/twonode_write.c" \
    -Wl,--no-as-needed -lurma -lurma_common -lpthread -ldl

extra_staged=()
for extra in $EXTRA_BINS; do
    [[ -f "$extra" ]] || die "EXTRA_BINS entry not found: $extra"
    is_arm64_elf "$extra" || die "EXTRA_BINS entry is not AArch64: $extra"
    extra_dst="$STAGE/usr/bin/$(basename "$extra")"
    cp -L "$extra" "$extra_dst"
    chmod 0755 "$extra_dst"
    extra_staged+=("$extra_dst")
done

# Recursively copy the dynamic runtime needed by the UMDK binaries/provider.
# Every candidate is verified as AArch64 so a cross-build on x86 cannot silently
# pull host libraries into the guest image.
SEARCH_DIRS=(
    "$(dirname "$LIBURMA")"
    "$(dirname "$LIBCOMMON")"
    "$ARM_BUILD"
    /usr/aarch64-linux-gnu/lib
    /usr/lib/aarch64-linux-gnu
    /lib/aarch64-linux-gnu
)
if [[ -n "$UMMU_SHIM" ]]; then
    SEARCH_DIRS+=("$(dirname "$UMMU_SHIM")")
fi

locate_library() {
    local soname="$1" root candidate
    for root in "${SEARCH_DIRS[@]}"; do
        [[ -d "$root" ]] || continue
        candidate="$(find -L "$root" -type f -name "$soname" -print -quit 2>/dev/null || true)"
        if [[ -n "$candidate" ]] && is_arm64_elf "$candidate"; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

optional_runtime=()
for optional_soname in libnss_files.so.2 libnss_dns.so.2 libresolv.so.2; do
    optional_src="$(locate_library "$optional_soname" || true)"
    if [[ -n "$optional_src" ]]; then
        cp -L "$optional_src" "$STAGE/lib/$optional_soname"
        optional_runtime+=("$STAGE/lib/$optional_soname")
    fi
done

queue=(
    "$STAGE/usr/bin/urma_admin"
    "$STAGE/usr/bin/urma_perftest"
    "$STAGE/usr/bin/k_smoke"
    "$STAGE/usr/bin/k_dataplane"
    "$STAGE/usr/bin/twonode_write"
    "$STAGE/lib/liburma.so.0"
    "$STAGE/lib/liburma_common.so.0"
    "$STAGE/lib/urma/liburma_openurma.so"
    "${extra_staged[@]}"
    "${optional_runtime[@]}"
)
if [[ -f "$STAGE/lib/urma/liburma-udma.so" ]]; then
    queue+=("$STAGE/lib/urma/liburma-udma.so" "$STAGE/lib/libummu.so.1")
fi
seen=' '
qpos=0
while (( qpos < ${#queue[@]} )); do
    elf="${queue[$qpos]}"
    qpos=$((qpos + 1))
    while read -r needed; do
        [[ -n "$needed" ]] || continue
        case "$seen" in *" $needed "*) continue ;; esac
        seen="$seen$needed "
        [[ -f "$STAGE/lib/$needed" ]] && continue
        src="$(locate_library "$needed" || true)"
        [[ -n "$src" ]] || die "cannot locate ARM64 runtime dependency '$needed' (needed by $elf)"
        cp -L "$src" "$STAGE/lib/$needed"
        queue+=("$STAGE/lib/$needed")
    done < <("$READELF" -d "$elf" 2>/dev/null |
        sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
done

interpreter="$("$READELF" -l "$STAGE/usr/bin/urma_admin" |
    sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p')"
[[ -n "$interpreter" ]] || die 'cannot determine ARM64 dynamic loader'
loader_name="$(basename "$interpreter")"
loader_dst="$STAGE$interpreter"
if [[ ! -f "$loader_dst" ]]; then
    loader_src="$(locate_library "$loader_name" || true)"
    [[ -n "$loader_src" ]] || die "cannot locate ARM64 dynamic loader: $loader_name"
    mkdir -p "$(dirname "$loader_dst")"
    cp -L "$loader_src" "$loader_dst"
fi

cp "$LAB_DIR/overlay/init" "$STAGE/init"
cp "$LAB_DIR/overlay/etc/profile" "$STAGE/etc/profile"
cp "$LAB_DIR/overlay/etc/passwd" "$STAGE/etc/passwd"
cp "$LAB_DIR/overlay/etc/group" "$STAGE/etc/group"
cp "$LAB_DIR/overlay/etc/hosts" "$STAGE/etc/hosts"
cp "$LAB_DIR/overlay/etc/nsswitch.conf" "$STAGE/etc/nsswitch.conf"
cp "$LAB_DIR/overlay/usr/local/bin/ou-help" "$STAGE/usr/local/bin/ou-help"
cp "$LAB_DIR/overlay/usr/local/bin/ou-status" "$STAGE/usr/local/bin/ou-status"
cp "$LAB_DIR/overlay/usr/local/bin/ou-smoke" "$STAGE/usr/local/bin/ou-smoke"
cp "$LAB_DIR/overlay/usr/local/bin/ou-dataplane" "$STAGE/usr/local/bin/ou-dataplane"
cp "$LAB_DIR/overlay/usr/local/bin/ou-peer-server" "$STAGE/usr/local/bin/ou-peer-server"
cp "$LAB_DIR/overlay/usr/local/bin/ou-peer-client" "$STAGE/usr/local/bin/ou-peer-client"
cp "$LAB_DIR/overlay/usr/local/bin/ou-enable-sync" "$STAGE/usr/local/bin/ou-enable-sync"
cp "$LAB_DIR/overlay/usr/local/bin/ou-net-up" "$STAGE/usr/local/bin/ou-net-up"
cp "$LAB_DIR/overlay/usr/local/bin/ou-cpu-switch" "$STAGE/usr/local/bin/ou-cpu-switch"
cp "$LAB_DIR/overlay/usr/local/bin/ou-lat-server" "$STAGE/usr/local/bin/ou-lat-server"
cp "$LAB_DIR/overlay/usr/local/bin/ou-lat-client" "$STAGE/usr/local/bin/ou-lat-client"
chmod 0755 "$STAGE/init" "$STAGE/usr/local/bin/"ou-*

mkdir -p "$(dirname "$OUT")"
tmp_out="$OUT.tmp.$$"
( cd "$STAGE" && find . -print0 | LC_ALL=C sort -z |
    cpio --null -o --format=newc --owner=0:0 2>/dev/null |
    gzip -n -9 > "$tmp_out" )
mv "$tmp_out" "$OUT"

manifest="${OUT%.cpio.gz}.manifest.txt"
sha256_file() {
    sha256sum "$1" | awk '{print $1}'
}
{
    printf 'kernel_release=%s\n' "$kernel_release"
    printf 'kernel_commit=%s\n' "$(git -C "$KSRC" rev-parse HEAD 2>/dev/null || printf unknown)"
    printf 'openurma_commit=%s\n' "$(git -C "$OPENURMA_ROOT" rev-parse HEAD 2>/dev/null || printf unknown)"
    printf 'umdk_commit=%s\n' "$(git -C "$OPENURMA_ROOT/integration/umdk/vendor/umdk" rev-parse HEAD 2>/dev/null || printf unknown)"
    printf 'kernel=%s\n' "$VMLINUX"
    printf 'kernel_sha256=%s\n' "$(sha256_file "$VMLINUX")"
    printf 'initramfs=%s\n' "$OUT"
    printf 'initramfs_sha256=%s\n' "$(sha256_file "$OUT")"
    printf 'busybox=%s\n' "$BUSYBOX_ARM64"
    printf 'modules=ipv6.ko ubcore.ko uburma.ko openurma_ubcore.ko'
    for ko in $EXTRA_MODULES; do printf ' %s' "$(basename "$ko")"; done
    printf '\n'
    printf 'providers=%s\n' "$available_providers"
    printf 'commands=urma_admin urma_perftest k_smoke k_dataplane twonode_write ou-dist-sync ou-cpu-switch ou-enable-sync ou-net-up ou-help ou-status ou-smoke ou-dataplane ou-peer-server ou-peer-client ou-lat-server ou-lat-client\n'
    # These hashes bind the image to the mutable inputs most likely to change
    # during latency-model work. run-dual.sh compares them before boot, which
    # catches both an old image and a transiently truncated Docker bind mount.
    printf 'overlay_init_path=%s\n' "$LAB_DIR/overlay/init"
    printf 'overlay_init_sha256=%s\n' "$(sha256_file "$LAB_DIR/overlay/init")"
    printf 'ou_cpu_switch_path=%s\n' "$LAB_DIR/overlay/usr/local/bin/ou-cpu-switch"
    printf 'ou_cpu_switch_sha256=%s\n' "$(sha256_file "$LAB_DIR/overlay/usr/local/bin/ou-cpu-switch")"
    printf 'ou_lat_server_path=%s\n' "$LAB_DIR/overlay/usr/local/bin/ou-lat-server"
    printf 'ou_lat_server_sha256=%s\n' "$(sha256_file "$LAB_DIR/overlay/usr/local/bin/ou-lat-server")"
    printf 'ou_lat_client_path=%s\n' "$LAB_DIR/overlay/usr/local/bin/ou-lat-client"
    printf 'ou_lat_client_sha256=%s\n' "$(sha256_file "$LAB_DIR/overlay/usr/local/bin/ou-lat-client")"
    printf 'urma_perftest_path=%s\n' "$URMA_PERFTEST"
    printf 'urma_perftest_sha256=%s\n' "$(sha256_file "$URMA_PERFTEST")"
    printf 'k_smoke_source_path=%s\n' "$OPENURMA_ROOT/integration/umdk/tests/k_smoke.c"
    printf 'k_smoke_source_sha256=%s\n' "$(sha256_file "$OPENURMA_ROOT/integration/umdk/tests/k_smoke.c")"
    printf 'k_smoke_binary_sha256=%s\n' "$(sha256_file "$STAGE/usr/bin/k_smoke")"
    printf 'k_dataplane_source_path=%s\n' "$OPENURMA_ROOT/integration/umdk/tests/k_dataplane.c"
    printf 'k_dataplane_source_sha256=%s\n' "$(sha256_file "$OPENURMA_ROOT/integration/umdk/tests/k_dataplane.c")"
    printf 'k_dataplane_binary_sha256=%s\n' "$(sha256_file "$STAGE/usr/bin/k_dataplane")"
    printf 'provider_source_path=%s\n' "$PROVIDER_SRC"
    printf 'provider_source_sha256=%s\n' "$(sha256_file "$PROVIDER_SRC")"
    printf 'openurma_kmod_path=%s\n' "$OPENURMA_KO"
    printf 'openurma_kmod_sha256=%s\n' "$(sha256_file "$OPENURMA_KO")"
    printf 'ipv6_module_path=%s\n' "$IPV6_KO"
    printf 'ipv6_module_sha256=%s\n' "$(sha256_file "$IPV6_KO")"
    printf 'ubcore_module_path=%s\n' "$UBCORE_KO"
    printf 'ubcore_module_sha256=%s\n' "$(sha256_file "$UBCORE_KO")"
    printf 'uburma_module_path=%s\n' "$UBURMA_KO"
    printf 'uburma_module_sha256=%s\n' "$(sha256_file "$UBURMA_KO")"
    printf 'dist_sync_source_path=%s\n' "$LAB_DIR/tools/ou-dist-sync.c"
    printf 'dist_sync_source_sha256=%s\n' "$(sha256_file "$LAB_DIR/tools/ou-dist-sync.c")"
    printf 'cpu_switch_source_path=%s\n' "$LAB_DIR/tools/ou-cpu-switch.c"
    printf 'cpu_switch_source_sha256=%s\n' "$(sha256_file "$LAB_DIR/tools/ou-cpu-switch.c")"
    printf 'm5ops_dispatch_source_path=%s\n' "$M5OPS_DISPATCH_HEADER"
    printf 'm5ops_dispatch_source_sha256=%s\n' "$(sha256_file "$M5OPS_DISPATCH_HEADER")"
    if [[ -n "$STOCK_UDMA_PROVIDER" ]]; then
        printf 'stock_udma_provider_path=%s\n' "$STOCK_UDMA_PROVIDER"
        printf 'stock_udma_provider_sha256=%s\n' "$(sha256_file "$STOCK_UDMA_PROVIDER")"
        printf 'ummu_shim_path=%s\n' "$UMMU_SHIM"
        printf 'ummu_shim_sha256=%s\n' "$(sha256_file "$UMMU_SHIM")"
    fi
} > "$manifest"

note "wrote $OUT ($(du -h "$OUT" | awk '{print $1}'))"
note "manifest: $manifest"
note "kernel: $VMLINUX ($kernel_release)"
