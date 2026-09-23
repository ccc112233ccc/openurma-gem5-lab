#!/usr/bin/env bash
set -euo pipefail

lab_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
kernel_root="${KSRC:-$lab_root/oe66}"
arm_build="${ARM_BUILD:-$lab_root/artifacts/umdk-build}"
busybox="${BUSYBOX_ARM64:-/bin/busybox}"
output="${OUT:-$lab_root/out/official-udma.cpio.gz}"
bridge="$lab_root/official-udma/ub_v2m_bridge/openurma_ub_v2m.ko"

modules=(
    "$kernel_root/drivers/ub/ubfi/ubfi.ko"
    "$kernel_root/drivers/ub/ubus/ubus.ko"
    "$bridge"
    "$kernel_root/drivers/iommu/hisilicon/ummu-core/ummu-core.ko"
    "$kernel_root/drivers/iommu/hisilicon/ummu.ko"
    "$kernel_root/drivers/ub/ubus/vendor/hisilicon/hisi_ubus.ko"
    "$kernel_root/drivers/ub/ubase/ubase.ko"
    "$kernel_root/drivers/ub/urma/hw/udma/udma.ko"
    "$kernel_root/drivers/ub/urma/ubagg/ubagg.ko"
)

for module in "${modules[@]}"; do
    [[ -s "$module" ]] || {
        echo "missing official module: $module" >&2
        echo "run official-udma/build_modules.sh first" >&2
        exit 2
    }
done

printf -v extra_modules '%s ' "${modules[@]}"
KSRC="$kernel_root" \
ARM_BUILD="$arm_build" \
BUSYBOX_ARM64="$busybox" \
OUT="$output" \
EXTRA_MODULES="${extra_modules% }" \
    "$lab_root/build-interactive-initramfs.sh"

echo "official UDMA initramfs: $output"
