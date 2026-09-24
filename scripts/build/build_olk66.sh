#!/usr/bin/env bash
# Build the exact OLK-6.6 kernel/URMA modules used by OpenURMA Tier-G.
#
# Run this on ARM64 Linux, either natively or inside the build container. KSRC
# must live on a case-sensitive filesystem: a normal macOS bind mount corrupts Linux kernel
# paths that differ only by case (for example xt_MARK.h vs xt_mark.h).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAB_DIR="${OPENURMA_LAB_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
KSRC="${KSRC:-$LAB_DIR/oe66}"
OPENURMA_ROOT="${OPENURMA_ROOT:-$LAB_DIR/sources/OpenURMA}"
ARTIFACT_DIR="${ARTIFACT_DIR:-$LAB_DIR/artifacts/kernel}"
JOBS="${JOBS:-$(nproc)}"
ARCH="${ARCH:-arm64}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
EXPECTED_KERNEL_COMMIT="5078a3a23a1e1825ec136485173ec98668cdd640"

KMOD_DIR="$OPENURMA_ROOT/integration/umdk/kmod"
LINKAGE_H="$KSRC/arch/arm64/include/asm/linkage.h"
ASSEMBLER_H="$KSRC/arch/arm64/include/asm/assembler.h"
BTI_PATCH="${BTI_PATCH:-$LAB_DIR/patches/olk66-gem5-bti.patch}"

fail() {
    echo "[olk66] ERROR: $*" >&2
    exit 1
}

[[ -f "$KSRC/Makefile" ]] || fail "kernel tree not found at $KSRC"
[[ -f "$KMOD_DIR/Kbuild" ]] || fail "OpenURMA kmod not found at $KMOD_DIR"
command -v "${CROSS_COMPILE}gcc" >/dev/null || fail "missing ${CROSS_COMPILE}gcc"

have_commit="$(git -C "$KSRC" rev-parse HEAD)"
[[ "$have_commit" == "$EXPECTED_KERNEL_COMMIT" ]] || \
    fail "kernel commit is $have_commit; expected $EXPECTED_KERNEL_COMMIT"

# Refuse case-insensitive source trees.  Linux has tracked paths that differ
# only by case, so such a checkout can look successful while being corrupted.
case_probe="$(mktemp -d "$KSRC/.openurma-casecheck.XXXXXX")"
touch "$case_probe/lower"
touch "$case_probe/LOWER"
case_count="$(find "$case_probe" -maxdepth 1 -type f | wc -l)"
rm -f "$case_probe/lower" "$case_probe/LOWER"
rmdir "$case_probe"
[[ "$case_count" -eq 2 ]] || fail "$KSRC is not case-sensitive"

# Only the two documented gem5-compatibility edits are allowed in the source
# tree.  Preserve/refuse any unrelated user change.
while IFS= read -r changed; do
    [[ -z "$changed" ]] && continue
    case "$changed" in
        arch/arm64/include/asm/linkage.h|arch/arm64/include/asm/assembler.h) ;;
        *) fail "unexpected tracked kernel change: $changed" ;;
    esac
done < <(git -C "$KSRC" diff --name-only)

# gem5 v24 decodes the ARMv8 BTI hint as an unimplemented instruction.  The
# OLK-6.6 assembly macros emit it even when CONFIG_ARM64_BTI_KERNEL is off.
# Apply the reviewed compatibility patch once; a reverse check makes reruns
# idempotent without hiding any unrelated source-tree change.
[[ -f "$BTI_PATCH" ]] || fail "BTI compatibility patch not found: $BTI_PATCH"
if git -C "$KSRC" apply --reverse --check "$BTI_PATCH" 2>/dev/null; then
    echo "[olk66] BTI compatibility patch already applied"
else
    git -C "$KSRC" apply --check "$BTI_PATCH" || fail "BTI patch does not apply cleanly"
    git -C "$KSRC" apply "$BTI_PATCH"
fi
grep -q 'bti c ;' "$LINKAGE_H" && fail "BTI remains in linkage.h"
grep -q 'hint[[:space:]]*#\.L__bti_targets_\\targets' "$ASSEMBLER_H" && \
    fail "BTI hint remains in assembler.h"

echo "[olk66] configuring $have_commit"
make -C "$KSRC" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" defconfig
"$KSRC/scripts/config" --file "$KSRC/.config" \
    --enable CONFIG_UB \
    --module CONFIG_UB_URMA \
    --disable CONFIG_UB_UBUS \
    --disable CONFIG_UB_UBUS_BUS \
    --disable CONFIG_UB_UBUS_USI \
    --disable CONFIG_UB_UBFI \
    --disable CONFIG_UB_UBASE \
    --disable CONFIG_UB_CDMA \
    --disable CONFIG_UB_UDMA \
    --disable CONFIG_OBMM \
    --disable CONFIG_UB_SENTRY \
    --disable CONFIG_UB_SENTRY_REMOTE \
    --module CONFIG_IPV6 \
    --disable CONFIG_ARM64_BTI_KERNEL \
    --enable CONFIG_MODULES \
    --enable CONFIG_MODULE_UNLOAD \
    --enable CONFIG_BLK_DEV_INITRD \
    --enable CONFIG_RD_GZIP \
    --enable CONFIG_DEVTMPFS \
    --enable CONFIG_DEVTMPFS_MOUNT \
    --enable CONFIG_PROC_FS \
    --enable CONFIG_SYSFS \
    --enable CONFIG_TMPFS \
    --enable CONFIG_SERIAL_AMBA_PL011 \
    --enable CONFIG_SERIAL_AMBA_PL011_CONSOLE
make -C "$KSRC" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" olddefconfig

for required in \
    CONFIG_UB=y \
    CONFIG_UB_URMA=m \
    CONFIG_IPV6=m \
    CONFIG_MODULES=y \
    CONFIG_BLK_DEV_INITRD=y \
    CONFIG_DEVTMPFS=y \
    CONFIG_DEVTMPFS_MOUNT=y \
    CONFIG_SERIAL_AMBA_PL011=y \
    CONFIG_SERIAL_AMBA_PL011_CONSOLE=y; do
    grep -qx "$required" "$KSRC/.config" || fail "missing final config: $required"
done

# OLK-6.6 declares ARM64_BTI_KERNEL, but with GCC it is dependency-invisible
# (`depends on !CC_IS_GCC`) and olddefconfig omits both CONFIG_=y and the usual
# `# CONFIG_... is not set` line. Accept that `undef` state as disabled; only a
# real y/m state is unsafe for gem5 v24. The source-level bti->nop checks above
# remain mandatory regardless of Kconfig visibility.
bti_kernel_state="$("$KSRC/scripts/config" --file "$KSRC/.config" --state CONFIG_ARM64_BTI_KERNEL)"
case "$bti_kernel_state" in
    n|undef) ;;
    *) fail "CONFIG_ARM64_BTI_KERNEL must be disabled; state=$bti_kernel_state" ;;
esac

echo "[olk66] building Image + vmlinux ($JOBS jobs)"
make -C "$KSRC" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
    -j"$JOBS" Image vmlinux

# Generate scripts/module.lds before any scoped module build.  `make vmlinux`
# does not create it, and without this step modpost can succeed only for the
# final link to fail with a misleading "No rule to make target ... .ko".
make -C "$KSRC" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
    -j"$JOBS" modules_prepare

# `make vmlinux` emits vmlinux.symvers (exports from the built-in kernel), while
# a subsequent scoped `make M=... modules` expects those exports under the
# conventional root Module.symvers name. A full `make modules` would create the
# latter by appending every configured module, but that is unnecessary for the
# four modules this lab packages. Seed Module.symvers with the exact vmlinux
# export set; each scoped build still performs strict modpost and writes its own
# subtree Module.symvers for cross-module dependencies.
[[ -s "$KSRC/vmlinux.symvers" ]] || fail "missing $KSRC/vmlinux.symvers after vmlinux build"
install -m 0644 "$KSRC/vmlinux.symvers" "$KSRC/Module.symvers"

# Keep IPv6 modular to match the checked-in OpenURMA guest evidence.  ubcore's
# connection manager needs it, so the initramfs must load ipv6.ko first.
echo "[olk66] building ipv6.ko"
make -C "$KSRC" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
    -j"$JOBS" M=net/ipv6 ipv6.ko

# Building at drivers/ub aggregates ubcore/uburma exports into
# drivers/ub/Module.symvers, which the out-of-tree OpenURMA provider needs.
echo "[olk66] building official ubcore/uburma modules"
make -C "$KSRC" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
    -j"$JOBS" M=drivers/ub modules

UB_SYMVERS="$KSRC/drivers/ub/Module.symvers"
[[ -s "$UB_SYMVERS" ]] || fail "missing $UB_SYMVERS"

echo "[olk66] building openurma_ubcore.ko"
make -C "$KSRC" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
    M="$KMOD_DIR" KBUILD_EXTRA_SYMBOLS="$UB_SYMVERS" modules

VMLINUX="$KSRC/vmlinux"
IMAGE="$KSRC/arch/arm64/boot/Image"
IPV6_KO="$KSRC/net/ipv6/ipv6.ko"
UBCORE_KO="$KSRC/drivers/ub/urma/ubcore/ubcore.ko"
UBURMA_KO="$KSRC/drivers/ub/urma/uburma/uburma.ko"
UBAGG_KO="$KSRC/drivers/ub/urma/ubagg/ubagg.ko"
OPENURMA_KO="$KMOD_DIR/openurma_ubcore.ko"

for artifact in "$VMLINUX" "$IMAGE" "$IPV6_KO" "$UBCORE_KO" "$UBURMA_KO" "$UBAGG_KO" "$OPENURMA_KO"; do
    [[ -s "$artifact" ]] || fail "missing build artifact: $artifact"
done

kernel_release="$(make -s -C "$KSRC" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" kernelrelease)"
for module in "$IPV6_KO" "$UBCORE_KO" "$UBURMA_KO" "$UBAGG_KO" "$OPENURMA_KO"; do
    vermagic="$(strings "$module" | sed -n 's/^vermagic=//p' | head -1)"
    case "$vermagic" in
        "$kernel_release "*) ;;
        *) fail "vermagic mismatch: $module has '$vermagic', kernel is '$kernel_release'" ;;
    esac
done

mkdir -p "$ARTIFACT_DIR/modules"
install -m 0644 "$VMLINUX" "$ARTIFACT_DIR/vmlinux"
install -m 0644 "$IMAGE" "$ARTIFACT_DIR/Image"
install -m 0644 "$KSRC/.config" "$ARTIFACT_DIR/kernel.config"
install -m 0644 "$IPV6_KO" "$ARTIFACT_DIR/modules/ipv6.ko"
install -m 0644 "$UBCORE_KO" "$ARTIFACT_DIR/modules/ubcore.ko"
install -m 0644 "$UBURMA_KO" "$ARTIFACT_DIR/modules/uburma.ko"
install -m 0644 "$UBAGG_KO" "$ARTIFACT_DIR/modules/ubagg.ko"
install -m 0644 "$OPENURMA_KO" "$ARTIFACT_DIR/modules/openurma_ubcore.ko"
printf '%s\n' "$kernel_release" > "$ARTIFACT_DIR/kernelrelease.txt"
(
    cd "$ARTIFACT_DIR"
    sha256sum vmlinux Image kernel.config modules/*.ko > SHA256SUMS
)

echo "[olk66] PASS: $kernel_release"
echo "[olk66] persistent artifacts: $ARTIFACT_DIR"
