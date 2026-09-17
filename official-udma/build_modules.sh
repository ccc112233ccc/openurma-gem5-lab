#!/usr/bin/env bash
set -euo pipefail

kernel_root="${KSRC:-/opt/openurma-gem5-lab/oe66}"
fragment="${FRAGMENT:-/workspace/openurma-gem5-lab/official-udma/kernel.fragment}"
arch="${ARCH:-arm64}"
cross_compile="${CROSS_COMPILE:-aarch64-linux-gnu-}"
jobs="${JOBS:-8}"

[[ -d "$kernel_root" ]] || {
    echo "kernel source not found: $kernel_root" >&2
    exit 1
}
[[ -f "$fragment" ]] || {
    echo "kernel config fragment not found: $fragment" >&2
    exit 1
}

make_args=(ARCH="$arch" CROSS_COMPILE="$cross_compile")
cd "$kernel_root"

if [[ ! -f .config.pre-official-udma ]]; then
    cp .config .config.pre-official-udma
fi
scripts/kconfig/merge_config.sh -m .config "$fragment"
make "${make_args[@]}" olddefconfig

# UBUS and the builtin half of UMMU export symbols to the lower modules, so an
# Image link must precede targeted module modpost.  Building every enabled
# module is unnecessary and much slower.
make -j"$jobs" "${make_args[@]}" Image
cp vmlinux.symvers Module.symvers

build_module_dir() {
    local dir="$1"
    shift
    make -j"$jobs" "${make_args[@]}" M="$dir" "$@" modules
}

build_module_dir drivers/ub/ubfi
# The upstream vendor file memory.c uses the IRQ API without including its
# declaration. Keep the source pristine and make the standalone-module build
# deterministic by injecting the missing public kernel header at compile time.
make "${make_args[@]}" M=drivers/ub/ubus clean
build_module_dir drivers/ub/ubus \
    KCFLAGS="-include $kernel_root/include/linux/interrupt.h" \
    KBUILD_EXTRA_SYMBOLS="$kernel_root/drivers/ub/ubfi/Module.symvers"
build_module_dir drivers/iommu/hisilicon/ummu-core
build_module_dir drivers/ub/urma/ubcore
build_module_dir drivers/ub/urma/uburma \
    KBUILD_EXTRA_SYMBOLS="$kernel_root/drivers/ub/urma/ubcore/Module.symvers"
build_module_dir drivers/ub/ubase \
    KBUILD_EXTRA_SYMBOLS="$kernel_root/drivers/iommu/hisilicon/ummu-core/Module.symvers $kernel_root/drivers/ub/ubus/Module.symvers"
build_module_dir drivers/ub/urma/hw/udma \
    KBUILD_EXTRA_SYMBOLS="$kernel_root/drivers/iommu/hisilicon/ummu-core/Module.symvers $kernel_root/drivers/ub/ubase/Module.symvers $kernel_root/drivers/ub/urma/ubcore/Module.symvers"

artifacts=(
    drivers/ub/ubfi/ubfi.ko
    drivers/ub/ubus/ubus.ko
    drivers/ub/ubus/vendor/hisilicon/hisi_ubus.ko
    drivers/iommu/hisilicon/ummu-core/ummu-core.ko
    drivers/ub/ubase/ubase.ko
    drivers/ub/urma/ubcore/ubcore.ko
    drivers/ub/urma/uburma/uburma.ko
    drivers/ub/urma/hw/udma/udma.ko
)

for artifact in "${artifacts[@]}"; do
    [[ -s "$artifact" ]] || {
        echo "missing module: $kernel_root/$artifact" >&2
        exit 1
    }
    printf '%s\n' "$kernel_root/$artifact"
done
