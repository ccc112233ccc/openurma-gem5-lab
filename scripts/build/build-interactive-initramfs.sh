#!/usr/bin/env bash
# Build the official ARM64 UDMA initramfs.
#
# This script intentionally does not download anything or modify source trees.
# It consumes an already-built OLK-6.6 tree and an already cross-built UMDK
# tree, persists selected boot artifacts, and packages the unmodified official
# UMDK provider and tools with a static ARM64 BusyBox.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAB_DIR="${LAB_DIR:-${UBSIM_LAB_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}}"
OVERLAY_DIR="${OVERLAY_DIR:-$LAB_DIR/overlay}"
TOOLS_DIR="${TOOLS_DIR:-$LAB_DIR/tools}"
UMDK_SRC="${UMDK_SRC:-$LAB_DIR/sources/umdk}"
GEM5_ROOT="${GEM5_ROOT:-$LAB_DIR/gem5}"
KSRC="${KSRC:-}"
ARM_BUILD="${ARM_BUILD:-}"
BUSYBOX_ARM64="${BUSYBOX_ARM64:-}"
OUT="${OUT:-$LAB_DIR/out/official-udma.cpio.gz}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
EXTRA_BINS="${EXTRA_BINS:-}"
EXTRA_LIBRARY_DIRS="${EXTRA_LIBRARY_DIRS:-}"
ARM64_SYSROOT="${UBSIM_ARM64_SYSROOT:-}"
EXTRA_MODULES="${EXTRA_MODULES:-}"
STOCK_UDMA_PROVIDER="${STOCK_UDMA_PROVIDER:-}"
UMMU_LIBRARY="${UMMU_LIBRARY:-}"
UBAGG_PROVIDER="${UBAGG_PROVIDER:-}"
UBAGG_CLI="${UBAGG_CLI:-}"
LIBTPSA="${LIBTPSA:-}"
DROPBEAR="${DROPBEAR:-}"
DROPBEARKEY="${DROPBEARKEY:-}"
KERNEL_BUNDLE_DIR="${KERNEL_BUNDLE_DIR:-$LAB_DIR/artifacts/kernel}"

if [[ -z "$DROPBEAR" ]]; then
    if [[ -n "$ARM64_SYSROOT" && -f "$ARM64_SYSROOT/usr/sbin/dropbear" ]]; then
        DROPBEAR="$ARM64_SYSROOT/usr/sbin/dropbear"
    else
        DROPBEAR=/usr/sbin/dropbear
    fi
fi
if [[ -z "$DROPBEARKEY" ]]; then
    if [[ -n "$ARM64_SYSROOT" && -f "$ARM64_SYSROOT/usr/bin/dropbearkey" ]]; then
        DROPBEARKEY="$ARM64_SYSROOT/usr/bin/dropbearkey"
    else
        DROPBEARKEY=/usr/bin/dropbearkey
    fi
fi

usage() {
    cat <<'EOF'
Usage:
  KSRC=/path/to/olk-6.6 \
  ARM_BUILD=/path/to/umdk-arm-build \
  BUSYBOX_ARM64=/path/to/static-arm64-busybox \
    ./lab build initramfs

Optional environment:
  UMDK_SRC       official UMDK checkout (default: sources/umdk)
  OUT            output .cpio.gz path
  CROSS_COMPILE  tool prefix (default: aarch64-linux-gnu-)
  EXTRA_BINS     space-separated extra ARM64 executables to place in /usr/bin
  EXTRA_LIBRARY_DIRS
                 space-separated directories searched for dependencies of
                 EXTRA_BINS (for example a Mooncake build's libasio.so)
  EXTRA_MODULES  space-separated extra kernel modules to place in /lib/modules
  STOCK_UDMA_PROVIDER
                 official liburma-udma.so (auto-detected in ARM_BUILD)
  UMMU_LIBRARY   official libummu.so.1 paired with STOCK_UDMA_PROVIDER
  UBAGG_PROVIDER official liburma_ubagg.so (auto-detected in ARM_BUILD)
  UBAGG_CLI      official ubagg_cli executable (auto-detected in ARM_BUILD)
  LIBTPSA        official UVS control-plane library (auto-detected in ARM_BUILD)
  DROPBEAR       ARM64 Dropbear SSH server (defaults to the ARM64 sysroot,
                 then /usr/sbin/dropbear)
  DROPBEARKEY    matching ARM64 host-key utility (same selection rule)
  KERNEL_BUNDLE_DIR
                 persistent workspace bundle for the exact vmlinux/in-tree
                 modules packaged in the image (default: artifacts/kernel)
  UBCORE_KO, UBURMA_KO, IPV6_KO
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
[[ -d "$UMDK_SRC/.git" || -f "$UMDK_SRC/.git" ]] || die "UMDK checkout not found: $UMDK_SRC"
[[ -f "$GEM5_ROOT/include/gem5/m5ops.h" ]] || die "not a gem5 checkout: $GEM5_ROOT"
[[ -f "$KSRC/Makefile" && -f "$KSRC/vmlinux" ]] || die "KSRC is not a built kernel tree: $KSRC"
[[ -f "$BUSYBOX_ARM64" ]] || die "BusyBox not found: $BUSYBOX_ARM64"
[[ -f "$DROPBEAR" ]] || die "Dropbear SSH server not found: $DROPBEAR"
[[ -f "$DROPBEARKEY" ]] || die "Dropbear key utility not found: $DROPBEARKEY"

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
is_arm64_elf "$DROPBEAR" || die 'DROPBEAR is not an AArch64 ELF binary'
is_arm64_elf "$DROPBEARKEY" || die 'DROPBEARKEY is not an AArch64 ELF binary'

REQUIRED_APPLETS='sh ash mount umount mkdir ln ls cat echo dmesg insmod rmmod lsmod ip hostname uname ps grep sed awk sleep sync poweroff reboot halt setsid cttyhack chmod touch taskset'
if busybox_applets="$($BUSYBOX_ARM64 --list 2>/dev/null)"; then
    for applet in $REQUIRED_APPLETS; do
        grep -qx "$applet" <<<"$busybox_applets" ||
            die "BUSYBOX_ARM64 does not provide the required '$applet' applet"
    done
fi

if [[ -n "${KERNEL_RELEASE:-}" ]]; then
    kernel_release=$KERNEL_RELEASE
elif [[ -f "$KSRC/include/config/auto.conf" ]]; then
    kernel_release="$(make -s -C "$KSRC" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" kernelrelease)"
elif [[ -r "$KERNEL_BUNDLE_DIR/kernelrelease.txt" ]]; then
    # A cleaned source tree may retain only the immutable kernel bundle.  The
    # release belongs to the bundled vmlinux/modules and is therefore a more
    # reliable packaging input than regenerating configuration in-place.
    kernel_release="$(sed -n '1p' "$KERNEL_BUNDLE_DIR/kernelrelease.txt")"
else
    die "kernel release unavailable: build KSRC or set KERNEL_RELEASE"
fi
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
if [[ -z "$UBAGG_PROVIDER" && -f "$ARM_BUILD/urma/lib/urma/bond/liburma_ubagg.so.0.0.1" ]]; then
    UBAGG_PROVIDER="$ARM_BUILD/urma/lib/urma/bond/liburma_ubagg.so.0.0.1"
fi
if [[ -z "$UBAGG_CLI" && -f "$ARM_BUILD/urma/tools/ubagg_cli/ubagg_cli" ]]; then
    UBAGG_CLI="$ARM_BUILD/urma/tools/ubagg_cli/ubagg_cli"
fi
if [[ -z "$LIBTPSA" && -f "$ARM_BUILD/urma/lib/uvs/core/libtpsa.so.0.0.1" ]]; then
    LIBTPSA="$ARM_BUILD/urma/lib/uvs/core/libtpsa.so.0.0.1"
fi
if [[ -n "$UBAGG_PROVIDER" || -n "$UBAGG_CLI" || -n "$LIBTPSA" ]]; then
    [[ -f "$UBAGG_PROVIDER" ]] || die "official ubagg provider not found: $UBAGG_PROVIDER"
    [[ -f "$UBAGG_CLI" ]] || die "official ubagg CLI not found: $UBAGG_CLI"
    [[ -f "$LIBTPSA" ]] || die "official UVS library not found: $LIBTPSA"
fi

# A BUILD_STOCK_UDMA=enable build places the official provider in ARM_BUILD
# and its official UMMU userspace dependency in a sibling build directory.
if [[ -z "$STOCK_UDMA_PROVIDER" && -f "$ARM_BUILD/urma/hw/udma/liburma-udma.so" ]]; then
    STOCK_UDMA_PROVIDER="$ARM_BUILD/urma/hw/udma/liburma-udma.so"
fi
if [[ -z "$UMMU_LIBRARY" && -f "${ARM_BUILD}-ummu-official/libummu.so.1" ]]; then
    UMMU_LIBRARY="${ARM_BUILD}-ummu-official/libummu.so.1"
fi
[[ -f "$STOCK_UDMA_PROVIDER" ]] ||
    die "official UDMA provider not found: $STOCK_UDMA_PROVIDER"
[[ -f "$UMMU_LIBRARY" ]] || die "official libummu not found: $UMMU_LIBRARY"

for f in "$KSRC/vmlinux" "$UBCORE_KO" "$UBURMA_KO" "$IPV6_KO" \
         "$LIBURMA" "$LIBCOMMON" "$URMA_ADMIN" "$URMA_PERFTEST"; do
    is_arm64_elf "$f" || die "not an AArch64 ELF artifact: $f"
done
is_arm64_elf "$STOCK_UDMA_PROVIDER" ||
    die "not an AArch64 ELF artifact: $STOCK_UDMA_PROVIDER"
is_arm64_elf "$UMMU_LIBRARY" || die "not an AArch64 ELF artifact: $UMMU_LIBRARY"
if [[ -n "$UBAGG_PROVIDER" ]]; then
    is_arm64_elf "$UBAGG_PROVIDER" || die "not an AArch64 ELF artifact: $UBAGG_PROVIDER"
    is_arm64_elf "$UBAGG_CLI" || die "not an AArch64 ELF artifact: $UBAGG_CLI"
    is_arm64_elf "$LIBTPSA" || die "not an AArch64 ELF artifact: $LIBTPSA"
fi

module_release() {
    "$READELF" -p .modinfo "$1" 2>/dev/null |
        sed -n 's/.*vermagic=\([^[:space:]]*\).*/\1/p' | head -n 1
}
for ko in "$UBCORE_KO" "$UBURMA_KO" "$IPV6_KO"; do
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

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/official-udma.XXXXXX")"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT
mkdir -p "$STAGE"/{bin,sbin,etc/dropbear,proc,sys,dev/pts,run,tmp,root,usr/bin,usr/sbin,usr/local/bin,lib/modules,lib/urma}
chmod 1777 "$STAGE/tmp"

cp -L "$BUSYBOX_ARM64" "$STAGE/bin/busybox"
chmod 0755 "$STAGE/bin/busybox"
for applet in sh ash mount umount mkdir mknod ln ls cat echo printf dmesg insmod rmmod lsmod \
              ip hostname uname ps grep sed awk sleep sync poweroff reboot halt setsid cttyhack \
              stty clear reset env id head tail hexdump devmem find chmod touch taskset; do
    ln -s busybox "$STAGE/bin/$applet"
done
ln -s /bin/busybox "$STAGE/sbin/init"

cp -L "$IPV6_KO" "$UBCORE_KO" "$UBURMA_KO" "$STAGE/lib/modules/"
for ko in $EXTRA_MODULES; do
    cp -L "$ko" "$STAGE/lib/modules/"
done
cp -L "$LIBURMA" "$STAGE/lib/liburma.so.0"
cp -L "$LIBCOMMON" "$STAGE/lib/liburma_common.so.0"
cp -L "$URMA_ADMIN" "$STAGE/usr/bin/urma_admin"
cp -L "$URMA_PERFTEST" "$STAGE/usr/bin/urma_perftest"
cp -L "$DROPBEAR" "$STAGE/usr/sbin/dropbear"
cp -L "$DROPBEARKEY" "$STAGE/usr/bin/dropbearkey"
chmod 0755 "$STAGE/usr/bin/urma_admin" "$STAGE/usr/bin/urma_perftest"
chmod 0755 "$STAGE/usr/sbin/dropbear" "$STAGE/usr/bin/dropbearkey"
if [[ -n "$UBAGG_PROVIDER" ]]; then
    cp -L "$UBAGG_PROVIDER" "$STAGE/lib/urma/liburma_ubagg.so"
    cp -L "$UBAGG_CLI" "$STAGE/usr/bin/ubagg_cli"
    cp -L "$LIBTPSA" "$STAGE/lib/libtpsa.so.0"
    chmod 0755 "$STAGE/lib/urma/liburma_ubagg.so" "$STAGE/usr/bin/ubagg_cli" \
        "$STAGE/lib/libtpsa.so.0"
fi

# A tiny honest wrapper around the collective dist-gem5 pseudo operation.
# It is intentionally named "toggle", not "on": invoking it a second time
# collectively disables synchronization, while invoking it on only one node
# quiesces that node waiting for its peer.
note "building ARM64 dist-sync pseudo-op helper"
scons -C "$GEM5_ROOT/util/m5" \
    "arm64.CROSS_COMPILE=$CROSS_COMPILE" build/arm64/out/libm5.a >/dev/null
M5_LIB="$GEM5_ROOT/util/m5/build/arm64/out/libm5.a"
M5OPS_DISPATCH_HEADER="$TOOLS_DIR/ubsim-m5ops.h"
[[ -f "$M5OPS_DISPATCH_HEADER" ]] || die "missing m5ops dispatcher: $M5OPS_DISPATCH_HEADER"
"$CC" -O2 -static -Wall -I"$GEM5_ROOT/include" \
    -I"$GEM5_ROOT/util/m5/src" -I"$TOOLS_DIR" \
    -o "$STAGE/usr/bin/ubsim-dist-sync" "$TOOLS_DIR/ubsim-dist-sync.c" "$M5_LIB"
is_arm64_elf "$STAGE/usr/bin/ubsim-dist-sync" || die "ubsim-dist-sync is not AArch64"
is_static_elf "$STAGE/usr/bin/ubsim-dist-sync" || die "ubsim-dist-sync is not static"

"$CC" -O2 -static -Wall -I"$GEM5_ROOT/include" \
    -I"$GEM5_ROOT/util/m5/src" -I"$TOOLS_DIR" \
    -o "$STAGE/usr/bin/ubsim-checkpoint" "$TOOLS_DIR/ubsim-checkpoint.c" "$M5_LIB"
is_arm64_elf "$STAGE/usr/bin/ubsim-checkpoint" || die "ubsim-checkpoint is not AArch64"
is_static_elf "$STAGE/usr/bin/ubsim-checkpoint" || die "ubsim-checkpoint is not static"

# A distinct switchcpu pseudo-op lets the host configuration replace the
# boot-fast AtomicSimpleCPUs with the configured ArmO3 CPUs at an explicit
# guest-visible boundary.  It is not a timer and does not guess when boot is
# complete.
"$CC" -O2 -static -Wall -I"$GEM5_ROOT/include" \
    -I"$GEM5_ROOT/util/m5/src" -I"$TOOLS_DIR" \
    -o "$STAGE/usr/bin/ubsim-cpu-switch-op" \
    "$TOOLS_DIR/ubsim-cpu-switch.c" "$M5_LIB"
is_arm64_elf "$STAGE/usr/bin/ubsim-cpu-switch-op" || \
    die "ubsim-cpu-switch-op is not AArch64"
is_static_elf "$STAGE/usr/bin/ubsim-cpu-switch-op" || \
    die "ubsim-cpu-switch-op is not static"

if [[ -n "$LIBTPSA" ]]; then
    "$CC" -O2 -Wall -Wl,-rpath,/lib \
        -I"$UMDK_SRC/src/urma/lib/uvs/core/include" \
        -L"$(dirname "$LIBTPSA")" \
        -o "$STAGE/usr/bin/ubsim-ubagg-topology" \
        "$TOOLS_DIR/ubsim-ubagg-topology-mxe.c" -Wl,--no-as-needed -ltpsa
    chmod 0755 "$STAGE/usr/bin/ubsim-ubagg-topology"
fi

cp -L "$STOCK_UDMA_PROVIDER" "$STAGE/lib/urma/liburma-udma.so"
cp -L "$UMMU_LIBRARY" "$STAGE/lib/libummu.so.1"
chmod 0755 "$STAGE/lib/urma/liburma-udma.so" "$STAGE/lib/libummu.so.1"
available_providers=udma
if [[ -f "$STAGE/lib/urma/liburma_ubagg.so" ]]; then
    available_providers="$available_providers ubagg"
fi
printf '%s\n' "$available_providers" > "$STAGE/etc/ubsim-available-providers"

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
if [[ -n "$ARM64_SYSROOT" ]]; then
    SEARCH_DIRS=(
        "$ARM64_SYSROOT/usr/lib/aarch64-linux-gnu"
        "$ARM64_SYSROOT/lib/aarch64-linux-gnu"
        "$ARM64_SYSROOT/usr/lib"
        "$ARM64_SYSROOT/lib"
        "${SEARCH_DIRS[@]}"
    )
fi
for extra_lib_dir in $EXTRA_LIBRARY_DIRS; do
    [[ -d "$extra_lib_dir" ]] || die "EXTRA_LIBRARY_DIRS entry not found: $extra_lib_dir"
    SEARCH_DIRS+=("$extra_lib_dir")
done
if [[ -n "$UMMU_LIBRARY" ]]; then
    SEARCH_DIRS+=("$(dirname "$UMMU_LIBRARY")")
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
    "$STAGE/lib/liburma.so.0"
    "$STAGE/lib/liburma_common.so.0"
    "$STAGE/lib/urma/liburma-udma.so"
    "$STAGE/lib/libummu.so.1"
    "$STAGE/usr/sbin/dropbear"
    "$STAGE/usr/bin/dropbearkey"
    "${extra_staged[@]}"
    "${optional_runtime[@]}"
)
if [[ -f "$STAGE/usr/bin/ubagg_cli" ]]; then
    queue+=("$STAGE/usr/bin/ubagg_cli" "$STAGE/usr/bin/ubsim-ubagg-topology" \
        "$STAGE/lib/urma/liburma_ubagg.so" "$STAGE/lib/libtpsa.so.0")
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

cp "$OVERLAY_DIR/init" "$STAGE/init"
cp "$OVERLAY_DIR/etc/profile" "$STAGE/etc/profile"
cp "$OVERLAY_DIR/etc/passwd" "$STAGE/etc/passwd"
cp "$OVERLAY_DIR/etc/group" "$STAGE/etc/group"
cp "$OVERLAY_DIR/etc/hosts" "$STAGE/etc/hosts"
cp "$OVERLAY_DIR/etc/nsswitch.conf" "$STAGE/etc/nsswitch.conf"
cp "$OVERLAY_DIR/etc/dropbear/dropbear_ed25519_host_key" \
    "$STAGE/etc/dropbear/dropbear_ed25519_host_key"
chmod 0600 "$STAGE/etc/dropbear/dropbear_ed25519_host_key"
cp "$OVERLAY_DIR/usr/local/bin/ubsim-help" "$STAGE/usr/local/bin/ubsim-help"
cp "$OVERLAY_DIR/usr/local/bin/ubsim-status" "$STAGE/usr/local/bin/ubsim-status"
cp "$OVERLAY_DIR/usr/local/bin/ubsim-enable-sync" "$STAGE/usr/local/bin/ubsim-enable-sync"
cp "$OVERLAY_DIR/usr/local/bin/ubsim-net-up" "$STAGE/usr/local/bin/ubsim-net-up"
cp "$OVERLAY_DIR/usr/local/bin/ubsim-cpu-switch" "$STAGE/usr/local/bin/ubsim-cpu-switch"
cp "$OVERLAY_DIR/usr/local/bin/ubsim-lat-server" "$STAGE/usr/local/bin/ubsim-lat-server"
cp "$OVERLAY_DIR/usr/local/bin/ubsim-lat-client" "$STAGE/usr/local/bin/ubsim-lat-client"
chmod 0755 "$STAGE/init" "$STAGE"/usr/local/bin/ubsim-*

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
    printf 'umdk_commit=%s\n' "$(git -C "$UMDK_SRC" rev-parse HEAD 2>/dev/null || printf unknown)"
    printf 'kernel=%s\n' "$VMLINUX"
    printf 'kernel_sha256=%s\n' "$(sha256_file "$VMLINUX")"
    printf 'initramfs=%s\n' "$OUT"
    printf 'initramfs_sha256=%s\n' "$(sha256_file "$OUT")"
    printf 'busybox=%s\n' "$BUSYBOX_ARM64"
    printf 'modules=ipv6.ko ubcore.ko uburma.ko'
    for ko in $EXTRA_MODULES; do printf ' %s' "$(basename "$ko")"; done
    printf '\n'
    printf 'providers=%s\n' "$available_providers"
    printf 'commands=urma_admin urma_perftest dropbear dropbearkey'
    if [[ -n "$UBAGG_PROVIDER" ]]; then
        printf ' ubagg_cli ubsim-ubagg-topology'
    fi
    printf ' ubsim-dist-sync ubsim-checkpoint ubsim-cpu-switch ubsim-enable-sync ubsim-net-up ubsim-help ubsim-status ubsim-lat-server ubsim-lat-client'
    for extra in $EXTRA_BINS; do printf ' %s' "$(basename "$extra")"; done
    printf '\n'
    extra_index=0
    for extra in $EXTRA_BINS; do
        printf 'extra_bin_%u_path=%s\n' "$extra_index" "$extra"
        printf 'extra_bin_%u_sha256=%s\n' "$extra_index" "$(sha256_file "$extra")"
        extra_index=$((extra_index + 1))
    done
    # These hashes bind the image to the mutable inputs most likely to change
    # during latency-model work. run-dual.sh compares them before boot, which
    # catches both an old image and a transiently truncated Docker bind mount.
    printf 'overlay_init_path=%s\n' "$OVERLAY_DIR/init"
    printf 'overlay_init_sha256=%s\n' "$(sha256_file "$OVERLAY_DIR/init")"
    printf 'ubsim_cpu_switch_path=%s\n' "$OVERLAY_DIR/usr/local/bin/ubsim-cpu-switch"
    printf 'ubsim_cpu_switch_sha256=%s\n' "$(sha256_file "$OVERLAY_DIR/usr/local/bin/ubsim-cpu-switch")"
    printf 'ubsim_lat_server_path=%s\n' "$OVERLAY_DIR/usr/local/bin/ubsim-lat-server"
    printf 'ubsim_lat_server_sha256=%s\n' "$(sha256_file "$OVERLAY_DIR/usr/local/bin/ubsim-lat-server")"
    printf 'ubsim_lat_client_path=%s\n' "$OVERLAY_DIR/usr/local/bin/ubsim-lat-client"
    printf 'ubsim_lat_client_sha256=%s\n' "$(sha256_file "$OVERLAY_DIR/usr/local/bin/ubsim-lat-client")"
    printf 'urma_perftest_path=%s\n' "$URMA_PERFTEST"
    printf 'urma_perftest_sha256=%s\n' "$(sha256_file "$URMA_PERFTEST")"
    if [[ -n "$UBAGG_PROVIDER" ]]; then
        printf 'ubagg_provider_path=%s\n' "$UBAGG_PROVIDER"
        printf 'ubagg_provider_sha256=%s\n' "$(sha256_file "$UBAGG_PROVIDER")"
        printf 'ubagg_cli_path=%s\n' "$UBAGG_CLI"
        printf 'ubagg_cli_sha256=%s\n' "$(sha256_file "$UBAGG_CLI")"
        printf 'libtpsa_path=%s\n' "$LIBTPSA"
        printf 'libtpsa_sha256=%s\n' "$(sha256_file "$LIBTPSA")"
        printf 'ubagg_topology_source_path=%s\n' "$TOOLS_DIR/ubsim-ubagg-topology-mxe.c"
        printf 'ubagg_topology_source_sha256=%s\n' "$(sha256_file "$TOOLS_DIR/ubsim-ubagg-topology-mxe.c")"
    fi
    printf 'ipv6_module_path=%s\n' "$IPV6_KO"
    printf 'ipv6_module_sha256=%s\n' "$(sha256_file "$IPV6_KO")"
    printf 'ubcore_module_path=%s\n' "$UBCORE_KO"
    printf 'ubcore_module_sha256=%s\n' "$(sha256_file "$UBCORE_KO")"
    printf 'uburma_module_path=%s\n' "$UBURMA_KO"
    printf 'uburma_module_sha256=%s\n' "$(sha256_file "$UBURMA_KO")"
    printf 'dist_sync_source_path=%s\n' "$TOOLS_DIR/ubsim-dist-sync.c"
    printf 'dist_sync_source_sha256=%s\n' "$(sha256_file "$TOOLS_DIR/ubsim-dist-sync.c")"
    printf 'checkpoint_source_path=%s\n' "$TOOLS_DIR/ubsim-checkpoint.c"
    printf 'checkpoint_source_sha256=%s\n' "$(sha256_file "$TOOLS_DIR/ubsim-checkpoint.c")"
    printf 'cpu_switch_source_path=%s\n' "$TOOLS_DIR/ubsim-cpu-switch.c"
    printf 'cpu_switch_source_sha256=%s\n' "$(sha256_file "$TOOLS_DIR/ubsim-cpu-switch.c")"
    printf 'm5ops_dispatch_source_path=%s\n' "$M5OPS_DISPATCH_HEADER"
    printf 'm5ops_dispatch_source_sha256=%s\n' "$(sha256_file "$M5OPS_DISPATCH_HEADER")"
    if [[ -n "$STOCK_UDMA_PROVIDER" ]]; then
        printf 'stock_udma_provider_path=%s\n' "$STOCK_UDMA_PROVIDER"
        printf 'stock_udma_provider_sha256=%s\n' "$(sha256_file "$STOCK_UDMA_PROVIDER")"
        printf 'ummu_library_path=%s\n' "$UMMU_LIBRARY"
        printf 'ummu_library_sha256=%s\n' "$(sha256_file "$UMMU_LIBRARY")"
    fi
} > "$manifest"

note "wrote $OUT ($(du -h "$OUT" | awk '{print $1}'))"
note "manifest: $manifest"
note "kernel: $VMLINUX ($kernel_release)"
